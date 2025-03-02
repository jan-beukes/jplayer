const std = @import("std");
const rl = @import("raylib");
const av = @import("av");
const VideoContext = @import("VideoContext.zig");
const c = @import("c.zig").c;

// some namespacing
const Thread = std.Thread;
const info = std.log.info;
const warn = std.log.warn;
const err = std.log.err;
const print = std.debug.print;
const assert = std.debug.assert;

const min_window_height = 200;
const default_window_height = 600;
const max_volume = 4.0;
const volume_step = 0.05;

const time_font_scale = 0.03;
const volume_bar_scale = 0.1;
const pause_scale = 0.1;

var pressed_last_frame = false;
var press_frame_count = 0;

fn usage(args: [][]u8) void {
    const executable = std.fs.path.basename(args[0]);
    print(
        \\USAGE: {s} [OPTIONS] <input file/url>
        \\yt-dlp: {s} [-- [yt-dlp options]] <url>
        \\
        \\Options:
        \\-q    quite
        \\
    , .{ executable, executable });
    std.process.exit(1);
}

// returns true if the video i
fn getYtFiles(alloc: std.mem.Allocator, url: [:0]u8, yt_dlp_args: ?[:0]u8) struct { [:0]u8, ?[:0]u8 } {
    _ = alloc;
    _ = yt_dlp_args;
    return .{ url, null };
}

fn mainLoop(surface: rl.Texture, ctx: *VideoContext) void {
    while (!rl.windowShouldClose()) {
        rl.clearBackground(rl.Color.black);

        if (!ctx.paused and ctx.video_active)
            ctx.update(surface);

        //---Events---

        rl.beginDrawing();
        rl.clearBackground(rl.Color.black);

        const screen_width = rl.getScreenWidth();
        const screen_height = rl.getScreenHeight();

        const dst = rl.Rectangle{
            .x = 0,
            .y = 0,
            .width = @floatFromInt(screen_width),
            .height = @floatFromInt(screen_height),
        };
        const src = rl.Rectangle{
            .x = 0,
            .y = 0,
            .width = @floatFromInt(surface.width),
            .height = @floatFromInt(surface.height),
        };
        if (ctx.video_active) {
            surface.drawPro(src, dst, rl.Vector2.zero(), 0, rl.Color.white);
        }

        rl.endDrawing();
    }
}

fn parseArgs(args: [][:0]u8, yt_dlp: *?[]u8) [:0]u8 {
    const inner = struct {
        var buf: [1024:0]u8 = undefined;
    };
    _ = inner;
    _ = yt_dlp;
    //var buf = inner.buf;

    if (args.len < 2) {
        usage(args);
    }

    const video_file = args[1];
    return video_file;
}

pub fn main() !void {
    const alloc = std.heap.c_allocator;

    const args = try std.process.argsAlloc(alloc);
    defer std.process.argsFree(alloc, args);

    var yt_dlp: ?[:0]u8 = null;
    const url = parseArgs(args, &yt_dlp);

    const video_file, const audio_file = getYtFiles(alloc, url, yt_dlp);

    // initialization
    var video_ctx = try VideoContext.init(alloc, video_file, audio_file);

    // launch threads
    video_ctx.video_active = true;
    video_ctx.io_active = true;
    video_ctx.v_decoder.active = true;
    video_ctx.a_decoder.active = true;
    var io_thread = Thread.spawn(.{}, VideoContext.ioThreadFunc, .{&video_ctx}) catch |e| {
        err("Failed to spawn io thread {}", .{e});
        std.process.exit(1);
    };
    io_thread.detach();
    var decode_thread = Thread.spawn(.{}, VideoContext.decodeThreadFunc, .{&video_ctx}) catch |e| {
        err("Failed to spawn decoding thread {}", .{e});
        std.process.exit(1);
    };
    decode_thread.detach();

    const vid_width = video_ctx.v_decoder.ctx.width;
    const vid_height = video_ctx.v_decoder.ctx.height;
    rl.setTraceLogLevel(.warning);
    rl.setConfigFlags(.{ .window_resizable = true });
    rl.initWindow(
        @divTrunc(default_window_height * vid_width, vid_height),
        default_window_height,
        "Epic",
    );
    rl.setTargetFPS(120);
    rl.initAudioDevice();
    rl.setWindowMinSize(@divTrunc(min_window_height * vid_width, vid_height), min_window_height);

    // Frame buffer
    const image = rl.Image{
        .width = vid_width,
        .height = vid_height,
        .mipmaps = 1,
        .format = .uncompressed_r8g8b8,
        .data = video_ctx.out_frame.data[0],
    };
    const surface = try image.toTexture();

    // audio
    try video_ctx.initAudio();

    mainLoop(surface, &video_ctx);
}
