#include <stdio.h>
#include <stdlib.h>

#include <raylib.h>
#include <libavcodec/avcodec.h>
#include <libavutil/avutil.h>
#include <libavutil/imgutils.h>
#include <libavformat/avformat.h>
#include <libswscale/swscale.h>

#define MIN_WINDOW_HEIGHT 200

#define ERROR(fmt, ...) ({ fprintf(stderr, "ERROR: "fmt"\n", ##__VA_ARGS__); exit(1); })
#define LOG(fmt, ...) printf("LOG: "fmt"\n", ##__VA_ARGS__)

typedef struct {
    AVFormatContext *format_ctx;

    // Codecs
    const AVCodec *v_codec;
    AVCodecContext *v_codec_ctx;
    int v_codec_index;

    const AVCodec *a_codec;
    AVCodecContext *a_codec_ctx;
    int a_codec_index;

    AVPacket *packet;
    AVFrame *frame;
    int fps;

    bool done;
    double current_time;
    double start_time;
    double pts;
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

    // find streams and set int context
    if (avformat_find_stream_info(ctx->format_ctx, NULL) < 0)
        ERROR("Could not find stream info");

    // get codecs and paramaters
    for (size_t i = 0; i < ctx->format_ctx->nb_streams; i++) {
        AVCodecParameters *codec_params = ctx->format_ctx->streams[i]->codecpar;
        const AVCodec *codec = avcodec_find_decoder(codec_params->codec_id);

        // initialize video codec
        if (codec_params->codec_type == AVMEDIA_TYPE_VIDEO) {
            LOG("Video %dx%d at %dfps", codec_params->width, codec_params->height,
                codec_params->framerate.num / codec_params->framerate.den);

            ctx->v_codec = codec;
            ctx->v_codec_ctx = avcodec_alloc_context3(ctx->v_codec);
            if (avcodec_parameters_to_context(ctx->v_codec_ctx, codec_params) < 0)
                ERROR("could not create video codec context");
            ctx->v_codec_ctx->framerate = ctx->format_ctx->streams[i]->avg_frame_rate;
            ctx->fps = ctx->v_codec_ctx->framerate.num / ctx->v_codec_ctx->framerate.den;
            ctx->v_codec_ctx->time_base = ctx->format_ctx->streams[i]->time_base;
            ctx->v_codec_index = i;
        } else if (codec_params->codec_type == AVMEDIA_TYPE_AUDIO){
            // initialize audio codec
            LOG("Audio %d chanels, sample rate %dHZ", 
                   codec_params->ch_layout.nb_channels, codec_params->sample_rate);
            ctx->a_codec = codec;
            ctx->a_codec_ctx = avcodec_alloc_context3(ctx->a_codec);
            if (avcodec_parameters_to_context(ctx->a_codec_ctx, codec_params) < 0)
                ERROR("could not create audio codec context");
            ctx->a_codec_index = i;
        }
            LOG("Codec %s ID %d bitrate %ld\n", codec->long_name, codec->id, codec_params->bit_rate);
    }

    // open the initialized codecs
    if (avcodec_open2(ctx->v_codec_ctx, ctx->v_codec, NULL) < 0)
        ERROR("Could not open vide0 codec");
     

    // allocate for the packet and frame components
    ctx->packet = av_packet_alloc();
    ctx->frame = av_frame_alloc();
    if (ctx->packet == NULL || ctx->frame == NULL)
        ERROR("Could not alloc packet or frame");
    return;
}

void update_surface(Texture surface, AVFrame *out_frame, AVContext *ctx, struct SwsContext *sws_ctx)
{
    if (ctx->done && frame_queue_len == 0) return;

    // time 
    if (ctx->start_time == 0) ctx->start_time = GetTime();

    ctx->current_time = GetTime();

    double pts_time = ctx->pts * 
        ((double)ctx->v_codec_ctx->time_base.num / ctx->v_codec_ctx->time_base.den);
    if ((ctx->current_time - ctx->start_time) >= pts_time) {
        // increment presentation time
        ctx->pts += ctx->v_codec_ctx->time_base.den / ctx->fps;

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

    // setup conversion
    enum AVPixelFormat format = ctx.v_codec_ctx->pix_fmt;
    int vid_width = ctx.v_codec_ctx->width, vid_height = ctx.v_codec_ctx->height;
    int num_bytes = av_image_get_buffer_size(AV_PIX_FMT_RGB24, vid_width, vid_height, 1);
    struct SwsContext *sws_ctx = sws_getContext(vid_width, vid_height, format, 
                                                vid_width, vid_height, AV_PIX_FMT_RGB24,
                                                SWS_BILINEAR, NULL, NULL, NULL);
    if (sws_ctx == NULL)
        ERROR("Failed to get sws context");

    AVFrame *out_frame = av_frame_alloc();
    unsigned char *frame_buf = malloc(num_bytes);
    int ret = av_image_fill_arrays(out_frame->data, out_frame->linesize,
                         frame_buf, AV_PIX_FMT_RGB24, vid_width, vid_height, 1);
    if (ret < 0) ERROR("Could not initialize out frame image");
    out_frame->width = vid_width;
    out_frame->height = vid_height;

    // Initialize raylib
    SetConfigFlags(FLAG_WINDOW_RESIZABLE|FLAG_VSYNC_HINT);
    SetTraceLogLevel(LOG_WARNING);
    InitWindow(vid_width, vid_height, ctx.format_ctx->url);
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
            if (ctx.packet->stream_index != ctx.v_codec_index) {
                av_packet_unref(ctx.packet);
                continue;
            }

            avcodec_send_packet(ctx.v_codec_ctx, ctx.packet);
            av_packet_unref(ctx.packet);
            // decoded frame
            int ret = avcodec_receive_frame(ctx.v_codec_ctx, ctx.frame);
            if (ret == AVERROR_EOF) {
                ctx.done = true;
                continue;
            }
            
            frame_queue[next] = av_frame_clone(ctx.frame);
            av_frame_unref(ctx.frame);
            frame_queue_len++;
        }

        // Rendering
        ClearBackground(BLACK);

        update_surface(surface, out_frame, &ctx, sws_ctx);

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
