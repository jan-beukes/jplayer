const std = @import("std");
const av = @import("av");
const rl = @import("raylib");
const sw = @import("sw.zig");

const info = std.log.info;
const warn = std.log.warn;
const err = std.log.err;
const print = std.debug.print;
const assert = std.debug.assert;

const VideoContext = @This();

allocator: std.mem.Allocator,

v_decoder: Decoder,
a_decoder: Decoder,
is_split: bool = false,

packets: *Queue(*av.Packet),
packets2: ?*Queue(*av.Packet) = null,

// conversion
out_frame: *av.Frame = undefined,
sws_ctx: *sw.SwsContext = undefined,
swr_ctx: ?*sw.SwrContext = null,

// audio
audio_stream: rl.AudioStream = undefined,
audio_buffer: []u8 = undefined,
buffer_size: u32 = 0,
volume: f32 = 1.0,
sample_size: i32 = 0,

// state
video_active: bool = false,
io_active: bool = false,
paused: bool = false,
muted: bool = false,

// clock
video_clock: i64 = 0,
audio_clock: i64 = 0,

pub fn ioThreadFunc(ctx: *VideoContext) void {
    var done = false;

    while (true) {
        if (rl.isWindowReady() and rl.windowShouldClose()) break;

        if (!ctx.is_split and !ctx.packets.full()) {
            const back = ctx.packets.back();
            ctx.v_decoder.format_ctx.read_frame(back) catch |e| {
                switch (e) {
                    error.EndOfFile => {
                        break;
                    },
                    else => warn("reading frame, {}", .{e}),
                }
            };
            ctx.packets.inc();
        } else if (ctx.packets2) |packets2| {
            if (!ctx.packets.full()) {
                const back = ctx.packets.back();
                ctx.v_decoder.format_ctx.read_frame(back) catch |e| switch (e) {
                    error.EndOfFile => {
                        if (done) break else done = true;
                    },
                    else => warn("reading video frame, {}", .{e}),
                };
                ctx.packets.inc();
            }
            if (packets2.full()) {
                const back = packets2.back();
                ctx.a_decoder.format_ctx.read_frame(back) catch |e| switch (e) {
                    error.EndOfFile => {
                        if (done) break else done = true;
                    },
                    else => warn("reading audio frame, {}", .{e}),
                };
                packets2.inc();
            }
        }
    }
    ctx.io_active = false;
}

pub fn decodeThreadFunc(ctx: *VideoContext) void {
    while (ctx.v_decoder.active or ctx.a_decoder.active) {
        if (rl.isWindowReady() and rl.windowShouldClose()) break;

        if (!ctx.is_split and !ctx.packets.empty()) {
            var packet = ctx.packets.peek();
            if (!ctx.v_decoder.frames.full() and packet.stream_index == ctx.v_decoder.index) {
                packet = ctx.packets.dequeue();
                ctx.v_decoder.decode(packet);
            } else if (!ctx.a_decoder.frames.full() and packet.stream_index == ctx.a_decoder.index) {
                packet = ctx.packets.dequeue();
                ctx.a_decoder.decode(packet);
            }
        } else if (ctx.is_split) {
            const packets2 = ctx.packets2.?;
            if (!ctx.packets.empty()) {
                const packet = ctx.packets.dequeue();
                ctx.v_decoder.decode(packet);
            }
            if (!packets2.empty()) {
                const packet = packets2.dequeue();
                ctx.a_decoder.decode(packet);
            }
        }
    }
}

pub fn update(self: *VideoContext, surface: rl.Texture) void {
    const v_frames = &self.v_decoder.frames;
    const a_frames = &self.a_decoder.frames;

    if (!self.v_decoder.active and
        !self.a_decoder.active and
        v_frames.empty() and a_frames.empty())
    {
        self.video_active = false;
        return;
    }

    if (!a_frames.empty() and rl.isAudioStreamProcessed(self.audio_stream)) {
        a_frames.mutex.lock();
        const frame = a_frames.dequeueNoLock();
        // convert from input sample format to interlaced FLT
        _ = sw.swr_convert(
            self.swr_ctx,
            @ptrCast(&self.audio_buffer.ptr),
            @intCast(self.audio_buffer.len),
            &frame.data,
            frame.nb_samples,
        );
        self.audio_clock += frame.nb_samples;
        rl.updateAudioStream(self.audio_stream, self.audio_buffer.ptr, frame.nb_samples);
        frame.unref();
        a_frames.mutex.unlock();
    }

    if (!v_frames.empty()) {
        v_frames.mutex.lock();
        const frame = v_frames.peekNoLock();
        const next_ts = @as(f64, @floatFromInt(frame.pts)) * self.v_decoder.ctx.time_base.q2d();
        const a_clock: f64 = @floatFromInt(self.audio_clock);
        const sample_rate: f64 = @floatFromInt(self.audio_stream.sampleRate);
        const audio_time = a_clock / sample_rate;

        if (audio_time >= next_ts) {
            self.video_clock = frame.pts;
            _ = v_frames.dequeueNoLock();
            v_frames.mutex.unlock();

            // convert to rgb
            _ = sw.sws_scale_frame(self.sws_ctx, self.out_frame, frame);
            rl.updateTexture(surface, self.out_frame.data[0]);
            frame.unref();
        } else {
            v_frames.mutex.unlock();
        }
    }
}

pub fn seek(self: *VideoContext, time_stamp: i64) void {
    _ = self;
    _ = time_stamp;
    //self.v_decoder.format_ctx.seek_frame()
}

pub fn initAudio(self: *VideoContext) !void {
    // Because of buffer filling issues when frame size is unknown we scan the frames for a value
    var buffer_size: u32 = @intCast(self.a_decoder.ctx.frame_size);
    const a_ctx = self.a_decoder.ctx;
    const a_frames = self.a_decoder.frames;
    if (buffer_size <= 0) {
        for (a_frames.items) |frame| {
            buffer_size = if (frame.nb_samples > buffer_size)
                @intCast(frame.nb_samples)
            else
                buffer_size;
        }
    }
    assert(buffer_size != 0);
    self.buffer_size = buffer_size;
    rl.setAudioStreamBufferSizeDefault(@intCast(buffer_size));
    self.audio_stream = try rl.loadAudioStream(
        @intCast(a_ctx.sample_rate),
        @intCast(self.sample_size),
        @intCast(a_ctx.ch_layout.nb_channels),
    );
    rl.setAudioStreamVolume(self.audio_stream, self.volume);
    const size: usize = buffer_size * self.audio_stream.channels * (self.audio_stream.sampleSize / 8);
    self.audio_buffer = try self.allocator.alloc(u8, size);
}

// setup audio and frame format conversion
fn initFrameConversion(self: *VideoContext) !void {
    const format = self.v_decoder.ctx.pix_fmt;
    const width = self.v_decoder.ctx.width;
    const height = self.v_decoder.ctx.height;
    const sws_ctx = sw.sws_getContext(
        width,
        height,
        format,
        width,
        height,
        .RGB24,
        .{ .BILINEAR = true },
        null,
        null,
        null,
    );
    if (sws_ctx == null) {
        err("Failed to get sws context", .{});
        std.process.exit(1);
    }
    self.sws_ctx = sws_ctx.?;

    var ret: i32 = undefined;
    self.out_frame = try av.Frame.alloc();
    ret = sw.av_image_alloc(
        self.out_frame.data[0..].ptr,
        self.out_frame.linesize[0..].ptr,
        width,
        height,
        .RGB24,
        1,
    );
    if (ret < 0) {
        err("Failed to allocate out frame buffer", .{});
        std.process.exit(1);
    }

    // Sample conversion
    const a_ctx = self.a_decoder.ctx;
    self.sample_size = 32;
    ret = sw.swr_alloc_set_opts2(
        &self.swr_ctx,
        &a_ctx.ch_layout,
        .FLT,
        a_ctx.sample_rate,
        &a_ctx.ch_layout,
        a_ctx.sample_fmt,
        a_ctx.sample_rate,
        0,
        null,
    );
    if (ret < 0) {
        err("Could not alloc swresample", .{});
        std.process.exit(1);
    }

    if (sw.swr_init(self.swr_ctx) < 0) {
        err("Could not alloc swresample", .{});
        std.process.exit(1);
    }
}

pub fn init(alloc: std.mem.Allocator, video_file: [:0]u8, audio_file: ?[:0]u8) !VideoContext {
    av.LOG.set_level(.ERROR);
    info("Loading Video...", .{});

    const format_ctx = av.FormatContext.open_input(video_file, null, null, null) catch |e| {
        err("Could not open video file {s}: {}", .{ video_file, e });
        std.process.exit(1);
    };

    const format_ctx2 = if (audio_file) |file| blk: {
        const ret = av.FormatContext.open_input(file, null, null, null) catch |e| {
            err("Could not open video file {s}: {}", .{ video_file, e });
            std.process.exit(1);
        };
        break :blk ret;
    } else format_ctx;
    const is_split = audio_file != null;

    // find the streams
    format_ctx.find_stream_info(null) catch |e| {
        err("Could not find stream info: {}", .{e});
        std.process.exit(1);
    };
    if (is_split) {
        format_ctx2.find_stream_info(null) catch |e| {
            err("Could not find audio stream info: {}", .{e});
            std.process.exit(1);
        };
    }
    info("Format {s}", .{format_ctx.iformat.long_name});

    // Packet queues
    const packets = try alloc.create(Queue(*av.Packet));
    packets.* = try Queue(*av.Packet).init();
    var packets2: *Queue(*av.Packet) = undefined;
    if (is_split) {
        packets2 = try alloc.create(Queue(*av.Packet));
        packets2.* = try Queue(*av.Packet).init();
    }
    const v_decoder = try Decoder.init(format_ctx, .VIDEO, packets);
    const a_decoder = try Decoder.init(format_ctx2, .AUDIO, packets2);

    var ctx = VideoContext{
        .v_decoder = v_decoder,
        .a_decoder = a_decoder,
        .allocator = alloc,
        .packets = packets,
        .packets2 = if (is_split) packets2 else null,
    };

    try ctx.initFrameConversion();
    return ctx;
}

pub fn deinit(self: *VideoContext) void {
    self.v_decoder.deinit();
    self.a_decoder.deinit();
    for (self.packets.items) |packet| {
        packet.free();
    }
    if (self.is_split) {}
    self.out_frame.free();
    sw.sws_freeContext(self.sws_ctx);
    sw.swr_free(&self.swr_ctx);
    self.allocator.free(self.audio_buffer);
}

pub const Decoder = struct {
    format_ctx: *av.FormatContext,
    ctx: *av.CodecContext,
    index: usize = 0,

    packets: *Queue(*av.Packet),
    frames: Queue(*av.Frame),

    fps: i32,
    duration: f64,

    active: bool = false,

    fn decode(self: *Decoder, packet: *av.Packet) void {
        if (!self.active) return;

        self.ctx.send_packet(packet) catch |e| {
            switch (e) {
                error.WouldBlock => warn("Packet not accepted", .{}),
                error.EndOfFile => warn("Decoder has been flushed", .{}),
                else => warn("sending packet, {}", .{e}),
            }
            return;
        };

        var frame = self.frames.back();
        while (!self.frames.full()) {
            self.ctx.receive_frame(frame) catch |e| switch (e) {
                error.EndOfFile => {
                    self.active = false;
                    return;
                },
                error.WouldBlock => break,
                else => {
                    warn("Receiving frame, {}", .{e});
                    break;
                },
            };
            self.frames.inc();
            frame = self.frames.back();
        }
    }

    fn init(format_ctx: *av.FormatContext, media_type: av.MediaType, packets: *Queue(*av.Packet)) !Decoder {
        var codec: *const av.Codec = undefined;
        var ret: i32 = 0;
        ret = av.av_find_best_stream(format_ctx, media_type, -1, -1, @ptrCast(&codec), 0);
        if (ret < 0) {
            err("Could not find video stream", .{});
            std.process.exit(1);
        }
        const index: usize = @intCast(ret);
        var ctx = try av.CodecContext.alloc(codec);
        ctx.parameters_to_context(format_ctx.streams[index].codecpar) catch |e| {
            err("Could not create video codec context {}", .{e});
            std.process.exit(1);
        };

        // setup fps and time_base
        var fps: i32 = 0;
        ctx.time_base = format_ctx.streams[index].time_base;
        const duration: f64 = @as(f64, @floatFromInt(format_ctx.duration)) / ctx.time_base.q2d();
        switch (media_type) {
            .VIDEO => {
                fps = @intFromFloat(format_ctx.streams[index].avg_frame_rate.q2d());
                info(
                    "Video {}x{} at {}fps",
                    .{ ctx.width, ctx.height, fps },
                );
            },
            .AUDIO => info(
                "Audio {} chanels, sample rate {}HZ, sample fmt {?s}",
                .{ ctx.ch_layout.nb_channels, ctx.sample_rate, ctx.sample_fmt.get_name() },
            ),
            else => {},
        }

        ctx.open(codec, null) catch |e| {
            err("Could not open {} codec {}", .{ media_type, e });
            std.process.exit(1);
        };

        const frames = try Queue(*av.Frame).init();

        return Decoder{
            .format_ctx = format_ctx,
            .ctx = ctx,
            .index = index,
            .packets = packets,
            .frames = frames,
            .fps = fps,
            .duration = duration,
        };
    }

    fn deinit(self: *Decoder) void {
        for (self.frames.items) |frame| {
            frame.free();
        }
        self.format_ctx.free();
    }
};

// Thread *safe* queue with locked operations
pub fn Queue(comptime T: type) type {
    return struct {
        const Self = @This();
        const default_packet_cap = 64;
        const default_frame_cap = 32;
        const cap = if (T == *av.Frame) default_frame_cap else default_packet_cap;

        items: [cap]T,
        windex: usize,
        rindex: usize,
        mutex: std.Thread.Mutex,

        pub fn init() !Self {
            var items: [cap]T = undefined;
            if (T == *av.Frame) {
                for (0..items.len) |i|
                    items[i] = try av.Frame.alloc();
            } else if (T == *av.Packet) {
                for (0..items.len) |i|
                    items[i] = try av.Packet.alloc();
            } else {
                @compileError("invalid type for Queue");
            }
            return .{
                .items = items,
                .windex = 0,
                .rindex = 0,
                .mutex = std.Thread.Mutex{},
            };
        }

        pub fn dequeue(self: *Self) T {
            self.mutex.lock();
            const ret = self.dequeueNoLock();
            self.mutex.unlock();
            return ret;
        }

        pub fn dequeueNoLock(self: *Self) T {
            const ret = self.items[self.rindex];
            self.rindex = (self.rindex + 1) % self.items.len;
            return ret;
        }

        pub fn peek(self: *Self) T {
            self.mutex.lock();
            const ret = self.peekNoLock();
            self.mutex.unlock();
            return ret;
        }

        pub fn peekNoLock(self: Self) T {
            return self.items[self.rindex];
        }

        pub fn inc(self: *Self) void {
            self.mutex.lock();
            self.windex = (self.windex + 1) % self.items.len;
            self.mutex.unlock();
        }

        pub fn back(self: *Self) T {
            self.mutex.lock();
            const ret = self.items[self.windex];
            self.mutex.unlock();
            return ret;
        }

        pub fn full(self: *Self) bool {
            self.mutex.lock();
            const ret = (self.windex + 1) % self.items.len == self.rindex;
            self.mutex.unlock();
            return ret;
        }

        pub fn empty(self: *Self) bool {
            self.mutex.lock();
            const ret = self.rindex == self.windex;
            self.mutex.unlock();
            return ret;
        }

        pub fn size(self: *Self) usize {
            self.mutex.lock();
            var ret: usize = 0;
            if (self.windex >= self.rindex) {
                ret = self.windex - self.rindex;
            } else {
                ret = self.items.len - self.rindex + self.windex + 1;
            }
            self.mutex.unlock();
            return ret;
        }
    };
}
