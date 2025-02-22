#include <stdio.h>
#include <stdlib.h>

#include <raylib.h>
#include <libavcodec/avcodec.h>
#include <libavutil/avutil.h>
#include <libavutil/imgutils.h>
#include <libavformat/avformat.h>
#include <libswscale/swscale.h>

#define MIN_WINDOW_HEIGHT 200
#define DEFAULT_WINDOW_HEIGHT 600

#define ERROR(fmt, ...) ({ fprintf(stderr, "ERROR: "fmt"\n", ##__VA_ARGS__); exit(1); })
#define LOG(fmt, ...) printf("LOG: "fmt"\n", ##__VA_ARGS__)

typedef struct {
    AVFormatContext *format_ctx;
    // Codecs
    AVCodecContext *v_ctx;
    int v_index;
    AVCodecContext *a_ctx;
    int a_index;

    AVPacket *packet;
    AVFrame *frame;

    bool done;
    int fps;
    double pts;
    double current_time;
    double start_time;
} AVContext;

// Globals

// frame queue ring buffer
AVFrame **frame_queue;
size_t frame_queue_len;
size_t frame_queue_cap;
int frame_queue_front;

bool pressed_last_frame = false;
int press_frame_count = 0;

void init_av_streaming(char *video_file, AVContext *ctx)
{
    // allocate format context and read format from file
    ctx->format_ctx = avformat_alloc_context();
    if (avformat_open_input(&ctx->format_ctx, video_file, NULL, NULL) != 0) {
        ERROR("Could not open video file %s", video_file);
    }
    LOG("Format %s\n", ctx->format_ctx->iformat->long_name);

    // find the streams in the format
    if (avformat_find_stream_info(ctx->format_ctx, NULL) < 0)
        ERROR("Could not find stream info");

    //---Codecs---
    const AVCodec *codec;
    // video codec
    ctx->v_index = av_find_best_stream(ctx->format_ctx, AVMEDIA_TYPE_VIDEO, -1, -1, &codec, 0);
    ctx->v_ctx = avcodec_alloc_context3(codec);
    if (avcodec_parameters_to_context(ctx->v_ctx, 
        ctx->format_ctx->streams[ctx->v_index]->codecpar) < 0)
        ERROR("could not create video codec context");

    AVRational framerate = ctx->format_ctx->streams[ctx->v_index]->avg_frame_rate;
    ctx->fps = framerate.num / ctx->v_ctx->framerate.den;
    ctx->v_ctx->time_base = ctx->format_ctx->streams[ctx->v_index]->time_base;
    LOG("Video %dx%d at %dfps", ctx->v_ctx->width, ctx->v_ctx->height, ctx->fps);
    LOG("Codec %s ID %d bitrate %ld\n", codec->long_name, codec->id, ctx->v_ctx->bit_rate);

    // initialize audio codec
    ctx->a_index = av_find_best_stream(ctx->format_ctx, AVMEDIA_TYPE_AUDIO, -1, ctx->v_index, &codec, 0);
    ctx->a_ctx = avcodec_alloc_context3(codec);
    if (avcodec_parameters_to_context(ctx->a_ctx,
        ctx->format_ctx->streams[ctx->a_index]->codecpar) < 0)
        ERROR("could not create audio codec context");

    LOG("Audio %d chanels, sample rate %dHZ", 
                   ctx->a_ctx->ch_layout.nb_channels, ctx->a_ctx->sample_rate);
    LOG("Codec %s ID %d bitrate %ld\n", codec->long_name, codec->id, ctx->v_ctx->bit_rate);

    // open the initialized codecs for use
    if (avcodec_open2(ctx->v_ctx, ctx->v_ctx->codec, NULL) < 0)
        ERROR("Could not open video codec");
     

    // allocate for the packet and frame components
    ctx->packet = av_packet_alloc();
    ctx->frame = av_frame_alloc();
    if (ctx->packet == NULL || ctx->frame == NULL)
        ERROR("Could not alloc packet or frame");
    return;
}

void deinit_av_streaming(AVContext *ctx)
{
    av_frame_free(&ctx->frame);
    av_packet_free(&ctx->packet);
    avcodec_free_context(&ctx->v_ctx);
    avcodec_free_context(&ctx->a_ctx);
    avformat_free_context(ctx->format_ctx);
}

// setup conversion
void init_frame_conversion(AVContext *ctx, AVFrame **out_frame, struct SwsContext **sws_ctx)
{
    enum AVPixelFormat format = ctx->v_ctx->pix_fmt;
    int vid_width = ctx->v_ctx->width, vid_height = ctx->v_ctx->height;
    int num_bytes = av_image_get_buffer_size(AV_PIX_FMT_RGB24, vid_width, vid_height, 1);
    struct SwsContext *local_sws_ctx = sws_getContext(vid_width, vid_height, format, 
                                                vid_width, vid_height, AV_PIX_FMT_RGB24,
                                                SWS_BILINEAR, NULL, NULL, NULL);
    if (sws_ctx == NULL)
        ERROR("Failed to get sws context");

    AVFrame *frame = av_frame_alloc();
    unsigned char *frame_buf = malloc(num_bytes);
    int ret = av_image_fill_arrays(frame->data, frame->linesize,
                         frame_buf, AV_PIX_FMT_RGB24, vid_width, vid_height, 1);
    if (ret < 0) ERROR("Could not initialize out frame image");
    frame->width = vid_width;
    frame->height = vid_height;

    *sws_ctx = local_sws_ctx;
    *out_frame = frame;
}

void update_surface(Texture surface, AVFrame *out_frame, AVContext *ctx, struct SwsContext *sws_ctx)
{
    if (ctx->done && frame_queue_len == 0) return;

    // time 
    if (ctx->start_time == 0) ctx->start_time = GetTime();

    ctx->current_time = GetTime();

    double pts_time = ctx->pts * av_q2d(ctx->v_ctx->time_base);
    if ((ctx->current_time - ctx->start_time) >= pts_time) {
        // increment presentation time
        ctx->pts += (double)ctx->v_ctx->time_base.den / ctx->fps;

        AVFrame **frame = &frame_queue[frame_queue_front];
        frame_queue_front = (frame_queue_front + 1) % frame_queue_cap;
        frame_queue_len--;

        // convert to rgb
        if ((*frame)->data[0] == NULL) ERROR("NULL Frame");
        sws_scale(sws_ctx, (const uint8_t *const *)((*frame)->data), (*frame)->linesize,
                  0, surface.height, out_frame->data, out_frame->linesize);
        UpdateTexture(surface, out_frame->data[0]);

        av_frame_free(frame);
    }

}

int main(int argc, char *argv[])
{
    if (argc < 2) {
        printf("USAGE: %s <input file>\n", argv[0]);
        return 1;
    }
    char *video_file = argv[1];

    AVContext ctx;
    init_av_streaming(video_file, &ctx);

    struct SwsContext *sws_ctx;
    AVFrame *out_frame;
    init_frame_conversion(&ctx, &out_frame, &sws_ctx);

    // Initialize raylib
    int vid_width = ctx.v_ctx->width, vid_height = ctx.v_ctx->height;
    SetConfigFlags(FLAG_WINDOW_RESIZABLE|FLAG_VSYNC_HINT);
    SetTraceLogLevel(LOG_WARNING);
    InitWindow(DEFAULT_WINDOW_HEIGHT * vid_width / vid_height,
               DEFAULT_WINDOW_HEIGHT, ctx.format_ctx->url);
    SetWindowMinSize(MIN_WINDOW_HEIGHT * vid_width / vid_height, MIN_WINDOW_HEIGHT);

    Image img = {
        .width = vid_width,
        .height = vid_height,
        .mipmaps = 1,
        .format = PIXELFORMAT_UNCOMPRESSED_R8G8B8,
        .data = out_frame->data[0],
    };
    Texture surface = LoadTextureFromImage(img);
    SetTextureFilter(surface, TEXTURE_FILTER_BILINEAR);

    frame_queue_cap = ctx.format_ctx->max_probe_packets/10;
    frame_queue = malloc(frame_queue_cap * sizeof(AVFrame *));
    frame_queue_len = 0;
    frame_queue_front = 0;

    ctx.start_time = 0, ctx.current_time = 0;
    ctx.done = false;
    while (!WindowShouldClose()) {
        int next = (frame_queue_front + frame_queue_len) % frame_queue_cap;
        // dont read frames if queue is full
        if (!ctx.done && (next + 1) != frame_queue_front) {
            av_read_frame(ctx.format_ctx, ctx.packet);
            // skip audio
            if (ctx.packet->stream_index != ctx.v_index) {
                av_packet_unref(ctx.packet);
                continue;
            }

            avcodec_send_packet(ctx.v_ctx, ctx.packet);
            av_packet_unref(ctx.packet);
            // decoded frame
            int ret = avcodec_receive_frame(ctx.v_ctx, ctx.frame);
            if (ret == AVERROR_EOF) {
                ctx.done = true;
                deinit_av_streaming(&ctx);
                continue;
            }
            
            frame_queue[next] = av_frame_clone(ctx.frame);
            av_frame_unref(ctx.frame);
            frame_queue_len++;
        }

        update_surface(surface, out_frame, &ctx, sws_ctx);

        // Rendering
        ClearBackground(BLACK);

        // Handle Window resizing
        Rectangle src = {0, 0, surface.width, surface.height};
        int screen_height = GetScreenHeight();
        int screen_width = GetScreenWidth();
        int height = screen_height;
        int width = screen_height * vid_width / vid_height;
        if (screen_width < width) {
            width = screen_width;
            height = screen_width * vid_height / vid_width;
        }
        int x = (screen_width - width) / 2;
        int y = (screen_height - height) / 2;
        Rectangle dst = {x, y, width, height};
        DrawTexturePro(surface, src, dst, (Vector2){0}, 0, WHITE);
        EndDrawing();

        // Events
        if (IsMouseButtonPressed(MOUSE_LEFT_BUTTON)) {
            if (pressed_last_frame) {
                if (IsWindowState(FLAG_WINDOW_MAXIMIZED))
                    ClearWindowState(FLAG_WINDOW_MAXIMIZED);
                else
                    SetWindowState(FLAG_WINDOW_MAXIMIZED);
            }
            pressed_last_frame = true;

        } else if (pressed_last_frame) {
            int fps = (int)(1 / GetFrameTime());
            if (++press_frame_count >= fps / 4) {
                press_frame_count = 0;
                pressed_last_frame = false;
            }
        }

    }
    sws_freeContext(sws_ctx);
    av_frame_free(&out_frame);

    return 0;
}
