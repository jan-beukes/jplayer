const std = @import("std");

// Although this function looks imperative, note that its job is to
// declaratively construct a build graph that will be executed by an external
// runner.
pub fn build(b: *std.Build) void {
    const target = b.standardTargetOptions(.{});
    const optimize = b.standardOptimizeOption(.{});

    const exe = b.addExecutable(.{
        .name = "jplayer",
        .root_source_file = b.path("src/main.zig"),
        .target = target,
        .optimize = optimize,
    });

    // Raylib
    const raylib_dep = b.dependency("raylib-zig", .{ .target = target, .optimize = optimize });
    const raylib = raylib_dep.module("raylib");
    const libraylib = raylib_dep.artifact("raylib");
    exe.linkLibrary(libraylib);
    exe.root_module.addImport("raylib", raylib);

    // ffmpeg
    const ffmpeg_dep = b.dependency("libffmpeg", .{ .target = target, .optimize = optimize });
    const av = ffmpeg_dep.module("av");
    const libav = ffmpeg_dep.artifact("ffmpeg");
    exe.addIncludePath(ffmpeg_dep.path(""));
    exe.linkLibrary(libav);
    exe.root_module.addImport("av", av);

    b.installArtifact(exe);

    const run_cmd = b.addRunArtifact(exe);
    run_cmd.step.dependOn(b.getInstallStep());

    if (b.args) |args| {
        run_cmd.addArgs(args);
    }

    const run_step = b.step("run", "Run the app");
    run_step.dependOn(&run_cmd.step);
}
