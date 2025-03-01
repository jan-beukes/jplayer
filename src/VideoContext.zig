const std = @import("std");
const av = @import("av");
const rl = @import("raylib");
const c = @cImport({
    @cInclude("libswscale/swscale.h");
    @cInclude("libswresample/swresample.h");
    @cInclude("libavutil/imgutils.h");
});

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

out_frame: *av.Frame = undefined,
sws_ctx: *c.SwsContext = undefined,
swr_ctx: *c.SwrContext = undefined,

audio_stream: rl.AudioStream = undefined,
audio_buffer: []u8 = undefined,
volume: f32 = 1.0,
sample_size: i32 = 0,

video_active: bool = false,
paused: bool = false,
muted: bool = false,

pub fn update(self: *VideoContext, surface: rl.Texture) void {
    _ = self;
    _ = surface;
}

pub fn initAudio(self: *VideoContext) !void {
    self.audio_buffer = try self.allocator.alloc(u8, 1024);
}

// setup audio and frame format conversion
fn initFrameConversion(self: *VideoContext) !void {
    const format = self.v_decoder.ctx.pix_fmt;
    const width = self.v_decoder.ctx.width;
    const height = self.v_decoder.ctx.height;
    const sws_ctx = c.sws_getContext(
        width,
        height,
        @intFromEnum(format),
        width,
        height,
        c.AV_PIX_FMT_RGB24,
        c.SWS_BILINEAR,
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
    self.out_frame.width = width;
    self.out_frame.height = height;
    self.out_frame.format = self.a_decoder.ctx.sample_fmt;
    ret = c.av_image_alloc(
        @ptrCast(&self.out_frame.data),
        @ptrCast(&self.out_frame.linesize),
        self.out_frame.width,
        self.out_frame.height,
        @intFromEnum(self.out_frame.format),
        1,
    );
    if (ret < 0) {
        err("Failed to allocate out frame buffer", .{});
        std.process.exit(1);
    }

    // Sample conversion
    const a_ctx = self.a_decoder.ctx;
    ret = c.swr_alloc_set_opts2(
        @ptrCast(&self.swr_ctx),
        @ptrCast(&a_ctx.ch_layout),
        c.AV_SAMPLE_FMT_FLT,
        a_ctx.sample_rate,
        @ptrCast(&a_ctx.ch_layout),
        @intFromEnum(a_ctx.sample_fmt),
        a_ctx.sample_rate,
        0,
        null,
    );
    if (ret < 0) {
        err("Could not alloc swresample", .{});
        std.process.exit(1);
    }

    if (c.swr_init(self.swr_ctx) < 0) {
        err("Could not alloc swresample", .{});
        std.process.exit(1);
    }
}

fn getYtFormat() void {}

pub fn init(alloc: std.mem.Allocator, video_file: [:0]u8, yt_dlp_args: ?[]u8) !VideoContext {
    _ = yt_dlp_args;

    av.LOG.set_level(.ERROR);
    info("Loading Video...", .{});

    const format_ctx = av.FormatContext.open_input(video_file, null, null, null) catch |e| {
        err("Could not open video file {s}: {}", .{ video_file, e });
        std.process.exit(1);
    };

    // find the streams
    format_ctx.find_stream_info(null) catch |e| {
        err("Could not find stream info: {}", .{e});
        std.process.exit(1);
    };
    info("Format {s}", .{format_ctx.iformat.long_name});
    const v_decoder = try Decoder.init(format_ctx, .VIDEO);
    const a_decoder = try Decoder.init(format_ctx, .AUDIO);
    var ctx = VideoContext{
        .v_decoder = v_decoder,
        .a_decoder = a_decoder,
        .allocator = alloc,
    };

    try ctx.initFrameConversion();
    return ctx;
}

pub fn deinit(self: *VideoContext) void {
    self.v_decoder.deinit();
    self.a_decoder.deinit();
    self.out_frame.free();
    c.sws_freeContext(self.sws_ctx);
    c.swr_free(&self.swr_ctx);
    self.allocator.free(self.audio_buffer);
}

pub const Decoder = struct {
    format_ctx: *av.FormatContext,
    ctx: *av.CodecContext,
    index: usize = 0,

    packets: Queue(*av.Packet),
    frames: Queue(*av.Frame),

    fps: i32,
    duration: f64,

    active: bool = false,

    fn init(format_ctx: *av.FormatContext, media_type: av.MediaType) !Decoder {
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
        const av_time_base: f64 = @floatFromInt(c.AV_TIME_BASE);
        const duration: f64 = @as(f64, @floatFromInt(format_ctx.duration)) / av_time_base;
        switch (media_type) {
            .VIDEO => {
                const framerate = format_ctx.streams[index].avg_frame_rate;
                fps = @divTrunc(framerate.num, framerate.den);
                info(
                    "Video {}x{} at {}fps",
                    .{ ctx.width, ctx.height, fps },
                );
            },
            .AUDIO => info(
                "Audio {} chanels, sample rate {}HZ, sample fmt {any}",
                .{ ctx.ch_layout.nb_channels, ctx.sample_rate, ctx.sample_fmt.get_name() },
            ),
            else => {},
        }

        ctx.open(codec, null) catch |e| {
            err("Could not open {} codec {}", .{ media_type, e });
            std.process.exit(1);
        };

        const packets = try Queue(*av.Packet).init();
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
        for (self.packets.items) |packet| {
            packet.free();
        }
        for (self.frames.items) |frame| {
            frame.free();
        }
        self.format_ctx.free();
    }
};

pub fn Queue(comptime T: type) type {
    return struct {
        const Self = @This();

        const default_capacity = 32;
        items: [default_capacity]T,
        windex: i32,
        rindex: i32,
        mutex: std.Thread.Mutex = std.Thread.Mutex{},
        locked: bool = false,

        pub fn init() !Self {
            var items: [default_capacity]T = undefined;
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
            };
        }

        pub fn lock(self: Self) void {
            if (self.locked) return;
            self.mutex.lock();
            self.locked = true;
        }

        pub fn unlock(self: Self) void {
            if (!self.locked) return;
            self.mutex.unlock();
            self.locked = false;
        }

        pub fn dequeue(self: Self) T {
            self.lock();
            const ret = self.items[self.rindex];
            self.rindex = (self.rindex + 1) % self.items.len;
            self.unlock();
            return ret;
        }

        pub fn inc(self: Self) void {
            self.lock();
            self.windex = (self.windex + 1) % self.items.len;
            self.unlock();
        }

        pub fn back(self: Self) T {
            self.lock();
            const ret = self.items[self.windex];
            self.unlock();
            return ret;
        }

        pub fn full(self: Self) bool {
            self.lock();
            const ret = (self.windex + 1) % self.items.len == self.rindex;
            self.unlock();
            return ret;
        }

        pub fn empty(self: Self) bool {
            self.lock();
            const ret = self.rindex == self.windex;
            self.unlock();
            return ret;
        }

        pub fn size(self: Self) i32 {
            if (self.windex >= self.rindex) {
                return self.windex - self.rindex;
            } else {
                return self.items.len - self.rindex + self.windex + 1;
            }
        }
    };
}
