pub const c = @cImport({
    @cInclude("libswscale/swscale.h");
    @cInclude("libavformat/avformat.h");
    @cInclude("libswresample/swresample.h");
    @cInclude("libavutil/imgutils.h");
});
