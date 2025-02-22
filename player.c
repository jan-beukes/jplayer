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

#define TIME_FONT_SCALE 0.04f
#define PAUSE_FONT_SCALE 0.1f


#define ERROR(fmt, ...) ({ fprintf(stderr, "ERROR: "fmt"\n", ##__VA_ARGS__); exit(1); })
#define LOG(fmt, ...) printf("LOG: "fmt"\n", ##__VA_ARGS__)
#define WARN(fmt, ...) printf("WARN: "fmt"\n", ##__VA_ARGS__)

// Queue stuff
#define QUEUE_EMPTY(Q) (Q.rindex == Q.windex)
#define QUEUE_FULL(Q) ((Q.windex + 1) % Q.cap == Q.rindex)
#define QUEUE_INC(Q) ({ Q.windex++; if (Q.windex >= Q.cap) Q.windex = 0; })
#define QUEUE_NEXT(Q) ({ Q.rindex++; if (Q.rindex >= Q.cap) Q.rindex = 0; })

typedef struct {
    AVFormatContext *format_ctx;
    // Codecs
    AVCodecContext *v_ctx;
    int v_index;
    AVCodecContext *a_ctx;
    int a_index;

    struct SwsContext *sws_ctx;

    AVPacket *packet;
    AVFrame *out_frame;

    bool video_active;
    bool decoding_active;
    bool paused;
    int fps;
    double video_time;
    double pause_time;
    double start_time;
} VideoContext;

#define QUEUE_CAP 256
typedef struct FrameQueue {
    AVFrame **items;
    int cap;
    int windex;
    int rindex;
} FrameQueue;

// Globals
FrameQueue fqueue = {0};

bool pressed_last_frame = false;
int press_frame_count = 0;

char *get_time_string(char *buf, int seconds)
{
    if (seconds < 60*60) {
        int s = seconds % 60;
        int m = seconds / 60;
        sprintf(buf, "%02d:%02d", m, s);
        return buf;
    } else {
        int s = seconds % 60;
        int m = seconds / 60;
        int h = seconds / (60*60);
        sprintf(buf, "%02d:%02d:%02d", h, m, s);
        return buf;
    }

}

void init_av_streaming(char *video_file, VideoContext *ctx)
{
    // allocate format context and read format from file
    ctx->format_ctx = avformat_alloc_context();
    if (avformat_open_input(&ctx->format_ctx, video_file, NULL, NULL) != 0) {
        // dont error if window is active
        if (IsWindowReady()) {
            LOG("Could not open video file %s", video_file);
            ctx->decoding_active = false;
            ctx->video_active = false;
            return;
        }
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
    if (ctx->packet == NULL)
        ERROR("Could not alloc packet or frame");

    ctx->decoding_active = true;
    ctx->video_active = true;
    return;
}

void deinit_av_streaming(VideoContext *ctx)
{
    for (int i = 0; i < fqueue.cap; i++)
        av_frame_free(&fqueue.items[i]);
    av_packet_free(&ctx->packet);
    av_frame_free(&ctx->out_frame);
    avcodec_free_context(&ctx->v_ctx);
    avcodec_free_context(&ctx->a_ctx);
    avformat_free_context(ctx->format_ctx);
    sws_freeContext(ctx->sws_ctx);
}

// setup conversion
void init_frame_conversion(VideoContext *ctx)
{
    enum AVPixelFormat format = ctx->v_ctx->pix_fmt;
    int vid_width = ctx->v_ctx->width, vid_height = ctx->v_ctx->height;
    int num_bytes = av_image_get_buffer_size(AV_PIX_FMT_RGB24, vid_width, vid_height, 1);
    ctx->sws_ctx = sws_getContext(vid_width, vid_height, format, 
                                                vid_width, vid_height, AV_PIX_FMT_RGB24,
                                                SWS_BILINEAR, NULL, NULL, NULL);
    if (ctx->sws_ctx == NULL)
        ERROR("Failed to get sws context");

    ctx->out_frame = av_frame_alloc();
    unsigned char *frame_buf = malloc(num_bytes);
    int ret = av_image_fill_arrays(ctx->out_frame->data, ctx->out_frame->linesize,
                         frame_buf, AV_PIX_FMT_RGB24, vid_width, vid_height, 1);
    if (ret < 0) ERROR("Could not initialize out frame image");
    ctx->out_frame->width = vid_width;
    ctx->out_frame->height = vid_height;
}

void update_frames(Texture surface, VideoContext *ctx)
{
    // Time updates
    if (ctx->start_time == 0.0) ctx->start_time = GetTime();
    ctx->video_time = GetTime() - ctx->start_time;

    AVFrame *frame = fqueue.items[fqueue.rindex];
    double next_ts = frame->pts * av_q2d(ctx->v_ctx->time_base);

    if (ctx->video_time >= next_ts) {
        QUEUE_NEXT(fqueue);

        // convert to rgb
        if (frame->data[0] == NULL) ERROR("NULL Frame");
        sws_scale(ctx->sws_ctx, (const uint8_t *const *)(frame->data), frame->linesize,
                  0, surface.height, ctx->out_frame->data, ctx->out_frame->linesize);
        UpdateTexture(surface, ctx->out_frame->data[0]);
        av_frame_unref(frame);
    }

}

bool decode(VideoContext *ctx)
{
    int ret;
    ret = av_read_frame(ctx->format_ctx, ctx->packet);
    if (ret != 0 && ret != AVERROR_EOF) {
        LOG("reading frame, %s", av_err2str(ret));
    }
    // skip audio
    if (ctx->packet->stream_index != ctx->v_index) {
        av_packet_unref(ctx->packet);
        return false;
    }

    ret = avcodec_send_packet(ctx->v_ctx, ctx->packet);
    av_packet_unref(ctx->packet);
    if (ret != 0 && ret != AVERROR_EOF) {
        WARN("sending packet, %s", av_err2str(ret));
        return false;
    }
    // decoded frame
    ret = avcodec_receive_frame(ctx->v_ctx, fqueue.items[fqueue.windex]);
    if (ret == AVERROR(EAGAIN)) return false;
    if (ret == AVERROR_EOF) {
        // Video done
        ctx->decoding_active = false;
        LOG("VIDEO DECODING DONE");
        return false;
    } else if (ret < 0) {
        WARN("receiving frame, %s", av_err2str(ret));
        return false;
    }
    QUEUE_INC(fqueue);

    return true;
}

int main(int argc, char *argv[])
{
    if (argc < 2) {
        printf("USAGE: %s <input file>\n", argv[0]);
        return 1;
    }
    char *video_file = argv[1];

    VideoContext ctx = {0};
    init_av_streaming(video_file, &ctx);
    init_frame_conversion(&ctx);

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
        .data = ctx.out_frame->data[0],
    };
    Texture surface = LoadTextureFromImage(img);
    SetTextureFilter(surface, TEXTURE_FILTER_BILINEAR);

    // frame queue
    fqueue.items = av_malloc_array(QUEUE_CAP, sizeof(AVFrame *));
    fqueue.cap = QUEUE_CAP;
    for (int i = 0; i < fqueue.cap; i++) fqueue.items[i] = av_frame_alloc();

    LOG("PLAYING...");
    while (!WindowShouldClose()) {

        // dont decode frames if queue is full
        if (ctx.decoding_active && !QUEUE_FULL(fqueue)) {
            if(!decode(&ctx)) continue;
        } else if (ctx.video_active && !ctx.decoding_active && QUEUE_EMPTY(fqueue)) {
            deinit_av_streaming(&ctx);
            ctx.video_active = false;
        }

        if (!ctx.paused && ctx.video_active)
            update_frames(surface, &ctx);

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
        if (ctx.video_active)
            DrawTexturePro(surface, src, dst, (Vector2){0}, 0, WHITE);

        //---UI-OVERLAY---
        float font_size = height * TIME_FONT_SCALE;
        // Time
        int duration = ctx.format_ctx->duration / AV_TIME_BASE;
        int current_time = ctx.video_time;
        char buf1[128], buf2[128];
        char *cur_time_str = get_time_string(buf1, current_time);
        char *dur_str = get_time_string(buf2, duration);
        const char *text = TextFormat("%s/%s", cur_time_str, dur_str);
        DrawText(text, x, y, font_size, RAYWHITE);

        // Pause
        if (ctx.paused) {
            font_size = height * PAUSE_FONT_SCALE;
            text = TextFormat("Paused");
            int text_width = MeasureText(text, font_size);
            x = (screen_width - text_width) / 2, y = (screen_height - (int)font_size) / 2;
            DrawText(text, x, y, font_size, RED);
        }

        EndDrawing();

        // Events
        if (IsKeyPressed(KEY_SPACE)) {
            if (ctx.paused) {
                ctx.start_time += (GetTime() - ctx.pause_time);
                ctx.paused = false;
            } else {
                ctx.pause_time = GetTime();
                ctx.paused = true;
            }
        }
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

    return 0;
}
