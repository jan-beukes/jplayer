const av = @import("av");

const sw = @This();

pub const SwsContext = opaque {};
pub const SwrContext = opaque {};

// Sws
pub extern fn sws_alloc_context() ?*SwsContext;
pub extern fn sws_init_context(sws_context: *SwsContext, srcFilter: ?*Filter, dstFilter: ?*Filter) c_int;
pub extern fn sws_freeContext(swsContext: ?*SwsContext) void;
pub extern fn sws_getContext(srcW: c_int, srcH: c_int, srcFormat: av.PixelFormat, dstW: c_int, dstH: c_int, dstFormat: av.PixelFormat, flags: Flags, srcFilter: ?*Filter, dstFilter: ?*Filter, ?[*]const f64) ?*SwsContext;
pub extern fn sws_scale(c: *SwsContext, srcSlice: [*]const [*]const u8, srcStride: [*]const c_int, srcSliceY: c_int, srcSliceH: c_int, dst: [*]const [*]u8, dstStride: [*]const c_int) c_int;
pub extern fn sws_scale_frame(c: *SwsContext, dst: *av.Frame, src: *const av.Frame) c_int;

// Swr
pub extern fn swr_alloc_set_opts2(
    ctx: *?*SwrContext,
    out_ch_layout: *av.ChannelLayout,
    out_sample_fmt: av.SampleFormat,
    out_sample_rate: c_int,
    in_ch_layout: *av.ChannelLayout,
    in_sample_fmt: av.SampleFormat,
    in_sample_rate: c_int,
    log_offet: c_int,
    log_ctx: ?*anyopaque,
) c_int;
pub extern fn swr_init(s: ?*SwrContext) c_int;
pub extern fn swr_free(s: *?*SwrContext) void;

pub extern fn swr_convert(s: ?*SwrContext, out: [*][*]u8, out_count: c_int, int: [*][*]u8, in_count: c_int) c_int;

// util
pub extern fn av_image_alloc(pointers: [*][*]u8, line_sizes: [*]c_int, w: c_int, h: c_int, pix_fmt: av.PixelFormat, alignment: c_int) c_int;

pub const Flags = packed struct(c_int) {
    FAST_BILINEAR: bool = false,
    BILINEAR: bool = false,
    BICUBIC: bool = false,
    X: bool = false,
    POINT: bool = false,
    AREA: bool = false,
    BICUBLIN: bool = false,
    GAUSS: bool = false,
    SINC: bool = false,
    LANCZOS: bool = false,
    SPLINE: bool = false,
    unused11: u1 = 0,
    PRINT_INFO: bool = false,
    /// not completely implemented
    /// internal chrominance subsampling info
    FULL_CHR_H_INT: bool = false,
    /// not completely implemented
    /// input subsampling info
    FULL_CHR_H_INP: bool = false,
    /// not completely implemented
    DIRECT_BGR: bool = false,
    SRC_V_CHR_DROP: u2 = 0,
    ACCURATE_RND: bool = false,
    BITEXACT: bool = false,
    unused20: u3 = 0,
    ERROR_DIFFUSION: bool = false,
    unused24: @Type(.{ .Int = .{ .signedness = .unsigned, .bits = @bitSizeOf(c_int) - 24 } }) = 0,
};

pub const Vector = extern struct {
    coeff: [*]f64,
    length: c_int,
};

pub const Filter = extern struct {
    lumH: ?*Vector,
    lumV: ?*Vector,
    chrH: ?*Vector,
    chrV: ?*Vector,
};
