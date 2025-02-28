#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <pthread.h>

#include <raylib.h>
#include <libavcodec/avcodec.h>
#include <libavutil/avutil.h>
#include <libavutil/imgutils.h>
#include <libavformat/avformat.h>
#include <libswscale/swscale.h>
#include <libswresample/swresample.h>

#define MIN_WINDOW_HEIGHT 200
#define DEFAULT_WINDOW_HEIGHT 600
#define MAX_VOLUME 5.0f
#define VOLUME_STEP 0.1f

#define TIME_FONT_SCALE 0.03f
#define VOLUME_BAR_SCALE 0.1f
#define PAUSE_SCALE 0.1f

#define ERROR(fmt, ...) ({ fprintf(stderr, "ERROR: "fmt"\n", ##__VA_ARGS__); exit(1); })
#define LOG(fmt, ...) ({ if (!quiet) printf("LOG: "fmt"\n", ##__VA_ARGS__); })
#define WARN(fmt, ...) ({ if (!quiet) printf("WARN: "fmt"\n", ##__VA_ARGS__); })
//#define endl "\n"

// Queue stuff
#define QUEUE_SIZE(Q) ({ \
    int __S; \
    if (Q.windex >= Q.rindex) __S = Q.windex - Q.rindex; \
    else __S = Q.cap - Q.rindex + Q.windex + 1; \
    __S; \
})

#define QUEUE_EMPTY(Q) ({ \
    pthread_mutex_lock(&Q.mutex); \
    bool _V = Q.rindex == Q.windex; \
    pthread_mutex_unlock(&Q.mutex); \
    _V; \
})

#define QUEUE_FULL(Q) ({ \
    pthread_mutex_lock(&Q.mutex); \
    bool _V = (Q.windex + 1) % Q.cap == Q.rindex; \
    pthread_mutex_unlock(&Q.mutex); \
    _V; \
})

#define QUEUE_BACK(Q, W) ({ \
    pthread_mutex_lock(&Q.mutex); \
    W = Q.items[Q.windex]; \
    pthread_mutex_unlock(&Q.mutex); \
})

#define QUEUE_INC(Q) ({ \
    pthread_mutex_lock(&Q.mutex); \
    if (++Q.windex >= Q.cap) Q.windex = 0; \
    pthread_mutex_unlock(&Q.mutex); \
})

#define DEQUEUE(Q) ({ \
    pthread_mutex_lock(&Q.mutex); \
    __typeof__(*Q.items) _V = Q.items[Q.rindex]; \
    if (++Q.rindex >= Q.cap) Q.rindex = 0; \
    pthread_mutex_unlock(&Q.mutex); \
    _V; \
})

#define QUEUE_PEEK(Q) ({ \
    pthread_mutex_lock(&Q.mutex); \
    __typeof__(*Q.items) _V = Q.items[Q.rindex]; \
    pthread_mutex_unlock(&Q.mutex); \
    _V; \
})

typedef struct {
    AVFormatContext *format_ctx;
    AVFormatContext *format_ctx2; // used for split audio

    // Codecs
    AVCodecContext *v_ctx;
    int v_index;
    AVCodecContext *a_ctx;
    int a_index;

    AVFrame *out_frame;
    struct SwsContext *sws_ctx;
    AudioStream audio_stream;
    int a_buffer_size;
    float volume;
    int sample_size;
    SwrContext *swr_ctx;

    // state stuff
    bool is_split;
    bool video_active;
    bool decoding_active;
    bool paused;
    bool muted;

    // clock
    int64_t video_clock;
    int64_t audio_clock;
    int fps;
    double duration;
} VideoContext;

#define FRAME_QUEUE_CAP 32
typedef struct FrameQueue {
    AVFrame **items;
    int cap;
    int windex;
    int rindex;
    pthread_mutex_t mutex;
} FrameQueue;

#define PACKET_QUEUE_CAP 64
typedef struct PacketQueue {
    AVPacket **items;
    int cap;
    int windex;
    int rindex;
    pthread_mutex_t mutex;
} PacketQueue;

// Globals
PacketQueue packets = {0};
PacketQueue packets2 = {0};
FrameQueue v_queue = {0};
FrameQueue a_queue = {0};
uint8_t *audio_buffer = NULL;

bool pressed_last_frame = false;
int press_frame_count = 0;

// flags
bool quiet = false;

char *get_time_string(char *buf, int seconds)
{
    if (seconds < 60*60) {
        int s = seconds % 60;
        int m = seconds / 60;
        sprintf(buf, "%02d:%02d", m, s);
        return buf;
    } else {
        int s = seconds % 60;
        int m = (seconds / 60) % 60;
        int h = seconds / (60*60) % 60;
        sprintf(buf, "%02d:%02d:%02d", h, m, s);
        return buf;
    }

}
 
// initialize format context from youtube url
#define BUF_MAX_LEN 2048
#define DEFAULT_ARGS "-f \"b*[height<=1080]+ba\""
void init_format_yt(VideoContext *ctx, char *video_file, char *yt_dlp_args)
{
    LOG("initializing youtube streaming...");

    char cmd[BUF_MAX_LEN];
    if (yt_dlp_args == NULL) yt_dlp_args = DEFAULT_ARGS;
    snprintf(cmd, BUF_MAX_LEN, "yt-dlp %s --get-url %s", yt_dlp_args, video_file);
    FILE *yt_stdout = popen(cmd, "r");
    if (yt_stdout == NULL) ERROR("popen");

    // Read the urls from yt-dlp stdout
    char url_buf[BUF_MAX_LEN];
    char url_buf2[BUF_MAX_LEN];
    fgets(url_buf, BUF_MAX_LEN, yt_stdout);
    char *ret = fgets(url_buf2, BUF_MAX_LEN, yt_stdout);

    if (pclose(yt_stdout) != 0) ERROR("failed to retrieve video with yt-dlp");

    // open the video file from url
    ctx->format_ctx = avformat_alloc_context();
    if (avformat_open_input(&ctx->format_ctx, url_buf, NULL, NULL) != 0)
        ERROR("Could not open youtube video %s", video_file);
    if (avformat_find_stream_info(ctx->format_ctx, NULL) < 0)
        ERROR("Could not find stream info");

    // if two streams were returned open the audio file into format_ctx2
    if (ret != NULL) {
        ctx->format_ctx2 = avformat_alloc_context();
        ctx->is_split = true;
        if (avformat_open_input(&ctx->format_ctx2, url_buf2, NULL, NULL) != 0)
            ERROR("Could not open youtube video %s", video_file);
        if (avformat_find_stream_info(ctx->format_ctx2, NULL) < 0)
            ERROR("Could not find stream info");
    }
}

// youtube also has the youtu.be domain
#define YT_DOMAINS {"https://www.youtu", "https://youtu", "youtu"}
void init_av_streaming(VideoContext *ctx, char *video_file, char *yt_dlp_args)
{
    av_log_set_level(AV_LOG_ERROR);
    //---Format---
    bool yt_url = false;
    const char *domains[] = YT_DOMAINS;
    for (int i = 0; i < 3; i++) {
        if (strncmp(video_file, domains[i], strlen(domains[i])) == 0)
            yt_url = true;
    }
    if (yt_url || yt_dlp_args != NULL) {
        init_format_yt(ctx, video_file, yt_dlp_args);
    } else {
        LOG("Loading Video");
        ctx->format_ctx = avformat_alloc_context();
        // allocate format context and read format from file
        if (avformat_open_input(&ctx->format_ctx, video_file, NULL, NULL) != 0)
            ERROR("Could not open video file %s", video_file);

        // find the streams in the format
        if (avformat_find_stream_info(ctx->format_ctx, NULL) < 0)
            ERROR("Could not find stream info");
    }
    LOG("Format %s%s", ctx->format_ctx->iformat->long_name,
        ctx->is_split ? " | split stream" : "");

    //---Codecs---
    int ret;
    const AVCodec *codec;

    // find video stream
    ret = av_find_best_stream(ctx->format_ctx, AVMEDIA_TYPE_VIDEO, -1, -1, &codec, 0);
    if (ret < 0 && ctx->is_split) {
        ret = av_find_best_stream(ctx->format_ctx2, AVMEDIA_TYPE_VIDEO, -1, -1, &codec, 0);
        if (ret < 0) ERROR("Could not find a video stream");
        // swap so that video is in ctx 1
        AVFormatContext *tmp = ctx->format_ctx;
        ctx->format_ctx = ctx->format_ctx2;
        ctx->format_ctx2 = tmp;
    }
    ctx->v_index = ret;
    ctx->v_ctx = avcodec_alloc_context3(codec);
    // create vido codec ctx
    if (avcodec_parameters_to_context(ctx->v_ctx, 
        ctx->format_ctx->streams[ctx->v_index]->codecpar) < 0)
        ERROR("could not create video codec context");

    // setup fps and time_base for vido ctx
    AVRational framerate = ctx->format_ctx->streams[ctx->v_index]->avg_frame_rate;
    ctx->fps = framerate.num / framerate.den;
    ctx->duration = (double)ctx->format_ctx->duration / AV_TIME_BASE;
    ctx->v_ctx->time_base = ctx->format_ctx->streams[ctx->v_index]->time_base;
    LOG("Video %dx%d at %dfps", ctx->v_ctx->width, ctx->v_ctx->height, ctx->fps);
    LOG("Codec %s ID %d", codec->long_name, codec->id);

    // initialize audio codec
    // if we are using seperated streams then audio must be in context 2
    AVFormatContext *audio_ctx = ctx->is_split ? ctx->format_ctx2 : ctx->format_ctx;

    ctx->a_index = av_find_best_stream(audio_ctx, AVMEDIA_TYPE_AUDIO, -1, ctx->v_index, &codec, 0);
    if (ctx->a_index < 0) ERROR("Could not find audio stream");
    ctx->a_ctx = avcodec_alloc_context3(codec);
    if (avcodec_parameters_to_context(ctx->a_ctx,
        audio_ctx->streams[ctx->a_index]->codecpar) < 0)
        ERROR("could not create audio codec context");
    if (ctx->a_index == ctx->v_index) ctx->a_index++;

    LOG("Audio %d chanels, sample rate %dHZ, sample fmt %s", 
        ctx->a_ctx->ch_layout.nb_channels, ctx->a_ctx->sample_rate, 
        av_get_sample_fmt_name(ctx->a_ctx->sample_fmt));
    LOG("Codec %s ID %d", codec->long_name, codec->id);

    // open the initialized codecs for use
    if (avcodec_open2(ctx->v_ctx, ctx->v_ctx->codec, NULL) < 0)
        ERROR("Could not open video codec");
    if (avcodec_open2(ctx->a_ctx, ctx->a_ctx->codec, NULL) < 0)
        ERROR("Could not open audio codec");
     
    ctx->decoding_active = true;
    ctx->video_active = true;
    return;
}

void deinit_av_streaming(VideoContext *ctx)
{
    // free queues
    for (int i = 0; i < v_queue.cap; i++)
        av_frame_free(&v_queue.items[i]);
    for (int i = 0; i < a_queue.cap; i++)
        av_frame_free(&a_queue.items[i]);
    for (int i = 0; i < packets.cap; i++)
        av_packet_free(&packets.items[i]);

    av_frame_unref(ctx->out_frame);
    av_frame_free(&ctx->out_frame);
    avcodec_free_context(&ctx->v_ctx);
    avcodec_free_context(&ctx->a_ctx);
    avformat_close_input(&ctx->format_ctx);
    avformat_free_context(ctx->format_ctx);
    if (ctx->is_split) {
        for (int i = 0; i < packets2.cap; i++)
            av_packet_free(&packets2.items[i]);
        avformat_close_input(&ctx->format_ctx2);
        avformat_free_context(ctx->format_ctx2);
    }
    sws_freeContext(ctx->sws_ctx);
    swr_close(ctx->swr_ctx);
    swr_free(&ctx->swr_ctx);
    av_free(audio_buffer);
    audio_buffer = NULL;
}

void init_frame_conversion(VideoContext *ctx)
{
    int ret;

    // Pixel conversion
    enum AVPixelFormat format = ctx->v_ctx->pix_fmt;
    int vid_width = ctx->v_ctx->width, vid_height = ctx->v_ctx->height;
    ctx->sws_ctx = sws_getContext(vid_width, vid_height, format, 
                                                vid_width, vid_height, AV_PIX_FMT_RGBA,
                                                SWS_BILINEAR, NULL, NULL, NULL);
    if (ctx->sws_ctx == NULL)
        ERROR("Failed to get sws context");
    ctx->out_frame = av_frame_alloc();
    ctx->out_frame->width = vid_width;
    ctx->out_frame->height = vid_height;

    // Sample conversion
    ret = swr_alloc_set_opts2(&ctx->swr_ctx, &ctx->a_ctx->ch_layout, AV_SAMPLE_FMT_FLT,
                        ctx->a_ctx->sample_rate, &ctx->a_ctx->ch_layout,
                        ctx->a_ctx->sample_fmt, ctx->a_ctx->sample_rate, 0, NULL);
    if (ret < 0) ERROR("Could not alloc swresample");
    if (swr_init(ctx->swr_ctx) < 0) ERROR("Could not init swresample");
}

void *io_thread_func(void *arg)
{
    VideoContext *ctx = (VideoContext *)arg;
    AVPacket *packet;
    bool done = false;
    int ret = 0;

    while (true) {
        if (IsWindowReady() && WindowShouldClose()) break;

        if (!QUEUE_FULL(packets)) {
            QUEUE_BACK(packets, packet);
            ret = av_read_frame(ctx->format_ctx, packet);
            if (ret == AVERROR_EOF) {
                done = true;
            }
            else if (ret < 0) {
                WARN("reading frame, %s", av_err2str(ret));
            } else {
                QUEUE_INC(packets);
            }
        }
        if (ctx->is_split) {
            if (QUEUE_FULL(packets2)) continue;
            QUEUE_BACK(packets2, packet);
            ret = av_read_frame(ctx->format_ctx2, packet);
            if (ret == AVERROR_EOF && done) {
                break;
            } else if (ret < 0 && ret != AVERROR_EOF) {
                WARN("reading audio frame, %s", av_err2str(ret));
            } else {
                packet->stream_index = ctx->a_index;
                QUEUE_INC(packets2);
            }
        } else if (done) break;
    }
    return NULL;
}

void decode(AVPacket *packet, FrameQueue *queue, AVCodecContext *codec_ctx, bool *done)
{
    // Video
    int ret;
    ret = avcodec_send_packet(codec_ctx, packet);
    if (ret != 0 && ret != AVERROR_EOF && ret != AVERROR(EAGAIN)) {
        WARN("sending packet, %s", av_err2str(ret));
        return;
    }
    AVFrame *frame;
    QUEUE_BACK((*queue), frame);
    while(!QUEUE_FULL((*queue)) && (ret = avcodec_receive_frame(codec_ctx, frame)) == 0) {
        QUEUE_INC((*queue));
        QUEUE_BACK((*queue), frame);
    }
    if (ret == AVERROR_EOF) {
        // Video done
        *done = true;
    } else if (ret < 0 && ret != AVERROR(EAGAIN)) {
        WARN("receiving frame, %s", av_err2str(ret));
    }
}

void *decode_thread_func(void *arg)
{
    VideoContext *ctx = (VideoContext *)arg;
    bool video_done = false, audio_done = false;

    while (ctx->decoding_active) {
        if (IsWindowReady() && WindowShouldClose()) break;

        AVPacket *packet;
        if (!QUEUE_EMPTY(packets)) {
            packet = QUEUE_PEEK(packets);
            if (!QUEUE_FULL(v_queue) && packet->stream_index == ctx->v_index) {
                packet = DEQUEUE(packets);
                decode(packet, &v_queue, ctx->v_ctx, &video_done);
            } else if (!ctx->is_split && !QUEUE_FULL(a_queue) && packet->stream_index == ctx->a_index) {
                packet = DEQUEUE(packets);
                decode(packet, &a_queue, ctx->a_ctx, &audio_done);
            }
        }
        if (ctx->is_split && !QUEUE_EMPTY(packets2) && !QUEUE_FULL(a_queue)) {
            packet = DEQUEUE(packets2);
            decode(packet, &a_queue, ctx->a_ctx, &audio_done);
        }

        if (audio_done && video_done) {
            ctx->decoding_active = false;
            LOG("VIDEO DECODING DONE");
            break;
        }
    }
    return NULL;
}

void start_threads(VideoContext *ctx)
{
    pthread_t io_thread, decode_thread;
    pthread_mutex_init(&packets.mutex, NULL);
    pthread_mutex_init(&v_queue.mutex, NULL);
    pthread_mutex_init(&a_queue.mutex, NULL);
    pthread_create(&io_thread, NULL, io_thread_func, ctx);
    pthread_create(&decode_thread, NULL, decode_thread_func, ctx);
    pthread_detach(io_thread);
    pthread_detach(decode_thread);
}

void update_frames(Texture surface, VideoContext *ctx)
{
    //LOG("%d %d %d %d", QUEUE_SIZE(packets), QUEUE_SIZE(packets2), QUEUE_SIZE(v_queue), QUEUE_SIZE(a_queue));
    AVFrame *frame;
    if (!QUEUE_EMPTY(a_queue) && IsAudioStreamProcessed(ctx->audio_stream)) {
        pthread_mutex_lock(&a_queue.mutex);
        frame = a_queue.items[a_queue.rindex];
        a_queue.rindex = (a_queue.rindex + 1) % a_queue.cap;

        swr_convert(ctx->swr_ctx, &audio_buffer, ctx->a_buffer_size,
                              (const uint8_t **)frame->data, frame->nb_samples);

        ctx->audio_clock += frame->nb_samples;
        UpdateAudioStream(ctx->audio_stream, audio_buffer, frame->nb_samples);
        av_frame_unref(frame);
        pthread_mutex_unlock(&a_queue.mutex);
    }
    if (!QUEUE_EMPTY(v_queue)) {
        pthread_mutex_lock(&v_queue.mutex);
        frame = v_queue.items[v_queue.rindex];
        assert(frame != NULL);
        double next_ts = frame->pts * av_q2d(ctx->v_ctx->time_base);
        double audio_time = (double)ctx->audio_clock / ctx->audio_stream.sampleRate;
        if (audio_time >= next_ts) {
            ctx->video_clock = frame->pts;
            v_queue.rindex = (v_queue.rindex + 1) % v_queue.cap;
            pthread_mutex_unlock(&v_queue.mutex);

            // convert to rgb and update video
            if (frame->data[0] == NULL) ERROR("NULL Frame");
            sws_scale_frame(ctx->sws_ctx, ctx->out_frame, frame);
            UpdateTexture(surface, ctx->out_frame->data[0]);

            av_frame_unref(frame);
        } else {
            pthread_mutex_unlock(&v_queue.mutex);
        }
    } 

}

void render_ui(VideoContext *ctx, Rectangle rect)
{
    int screen_width = GetScreenWidth(), screen_height = GetScreenHeight();
    //---UI-OVERLAY---
    float font_size = rect.height * TIME_FONT_SCALE;
    Color faded_black = {0, 0, 0, 100};

    // Time
    int current_time = ctx->audio_clock / ctx->audio_stream.sampleRate;
    char buf1[128], buf2[128];
    char *cur_time_str = get_time_string(buf1, current_time);
    char *dur_str = get_time_string(buf2, ctx->duration);
    const char *text = TextFormat("%s/%s", cur_time_str, dur_str);
    int text_width = MeasureText(text, font_size);
    float bottom = rect.y + rect.height;
    float right = rect.x + rect.width;
    DrawRectangle(right - text_width, bottom - font_size, text_width, font_size, faded_black);
    DrawText(text, right - text_width, bottom - font_size, font_size, RAYWHITE);

    // Volume
    // TODO: icon
    float padding = font_size*0.2f;
    float max_width = rect.width * VOLUME_BAR_SCALE;
    Rectangle volume_bar = {rect.x, bottom - font_size, max_width, font_size};
    DrawRectangleRounded(volume_bar, 0.4f, 20, faded_black);
    volume_bar = (Rectangle){
        .x = rect.x + padding,
        .y = bottom - font_size + padding,
        .width = (ctx->volume / MAX_VOLUME) * max_width - 2*padding,
        .height = font_size - 2*padding
    };
    Color color = ctx->muted ? GRAY : RAYWHITE;
    DrawRectangleRounded(volume_bar, 0.4f, 20, color);

    // Pause
    if (ctx->paused) {
        float pause_height = rect.height * PAUSE_SCALE;
        float pause_width = pause_height * 0.25;
        int y = (screen_height - pause_height) / 2;
        int x = screen_width / 2 - 2*(int)pause_width;
        DrawRectangle(x, y, pause_width, pause_height, RAYWHITE);
        DrawRectangleLines(x, y, pause_width, pause_height, BLACK);
        x += 2*pause_width;
        DrawRectangle(x, y, pause_width, pause_height, RAYWHITE);
        DrawRectangleLines(x, y, pause_width, pause_height, BLACK);
    }

}

void main_loop(VideoContext *ctx, Texture surface)
{
    PlayAudioStream(ctx->audio_stream);
    while (!WindowShouldClose()) {
        //float dt = GetFrameTime();

        if (!ctx->decoding_active && ctx->video_active && QUEUE_EMPTY(v_queue)) {
            // video finished
            deinit_av_streaming(ctx);
            ctx->video_active = false;
        }

        if (!ctx->paused && ctx->video_active)
            update_frames(surface, ctx);

        // Events
        if (IsKeyPressed(KEY_SPACE)) {
            ctx->paused = !ctx->paused;
        }
        float scroll = GetMouseWheelMoveV().y;
        if (IsKeyPressed(KEY_UP) || scroll > 0.0f) {
            float step = scroll ? VOLUME_STEP : VOLUME_STEP * 4.0f;
            ctx->volume += step;
            ctx->volume = ctx->volume > MAX_VOLUME ? MAX_VOLUME : ctx->volume;
            SetAudioStreamVolume(ctx->audio_stream, ctx->volume);
        } else if (IsKeyPressed(KEY_DOWN) || scroll < 0.0f) {
            float step = scroll ? VOLUME_STEP : VOLUME_STEP * 4.0f;
            ctx->volume -= step;
            ctx->volume = ctx->volume < 0.0f ? 0.0f : ctx->volume;
            SetAudioStreamVolume(ctx->audio_stream, ctx->volume);
        }
        if (IsKeyPressed(KEY_M)) {
            if (ctx->muted)
                SetAudioStreamVolume(ctx->audio_stream, ctx->volume);
            else
                SetAudioStreamVolume(ctx->audio_stream, 0.0f);
            ctx->muted = !ctx->muted;
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
            int fps = GetFPS();
            if (++press_frame_count >= fps / 4) {
                press_frame_count = 0;
                pressed_last_frame = false;
            }
        }
        // Rendering
        ClearBackground(BLACK);

        // Handle Window resizing
        Rectangle src = {0, 0, surface.width, surface.height};
        int screen_height = GetScreenHeight();
        int screen_width = GetScreenWidth();
        int height = screen_height;
        int width = screen_height * surface.width / surface.height;
        if (screen_width < width) {
            width = screen_width;
            height = screen_width * surface.height / surface.width;
        }
        int x = (screen_width - width) / 2;
        int y = (screen_height - height) / 2;
        Rectangle dst = {x, y, width, height};
        if (ctx->video_active)
            DrawTexturePro(surface, src, dst, (Vector2){0}, 0, WHITE);

        render_ui(ctx, dst);

        EndDrawing();

    }

}

#define USAGE() fprintf(stderr, \
"USAGE: %s [OPTIONS] <input file/url>\n" \
"yt-dlp: %s [-- [yt-dlp options]] <url>\n\n" \
"Options:\n" \
"-q\tquite\n" \
, argv[0], argv[0])

// return video file
char *parse_args(int argc, char *argv[], char **yt_dlp)
{
    static char yt_dlp_buf[1024];
    //---Arguments---
    if (argc < 2) {
        USAGE();
        exit(1);
    }
    *yt_dlp = NULL;
    // parse flags
    if (argc >= 3) {
        for (int i = 1; i < argc - 1; i++) {
            char *arg = argv[i];
            // yt-dlp args
            if (strcmp(arg, "--") == 0) {
                for (i++; i < argc - 1; i++) {
                    size_t len = strlen(yt_dlp_buf);
                    if (len < 1023) {
                        yt_dlp_buf[len] = ' ';
                        yt_dlp_buf[len + 1] = '\0';
                    }
                    size_t n = 1024 - len;
                    strncat(yt_dlp_buf, argv[i], n);
                }
                *yt_dlp = yt_dlp_buf;
            } else if (strcmp(arg, "-q") == 0) {
                quiet = true;
            } else {
                USAGE();
                exit(1);
            }
        }
    }
char *video_file = argv[argc - 1];
    return video_file;
}

int main(int argc, char *argv[])
{
    char *video_file;
    char *yt_dlp = NULL;
    video_file = parse_args(argc, argv, &yt_dlp);

    // Initialization
    VideoContext ctx = {0};
    init_av_streaming(&ctx, video_file, yt_dlp);
    init_frame_conversion(&ctx);

    // packets
    packets.cap = PACKET_QUEUE_CAP;
    packets.items = av_malloc_array(PACKET_QUEUE_CAP, sizeof(AVPacket *));
    for (int i = 0; i < packets.cap; i++) packets.items[i] = av_packet_alloc();
    if (ctx.is_split) {
        packets2.cap = PACKET_QUEUE_CAP;
        packets2.items = av_malloc_array(PACKET_QUEUE_CAP, sizeof(AVPacket *));
        for (int i = 0; i < packets2.cap; i++) packets2.items[i] = av_packet_alloc();
    }
    // video queue
    v_queue.cap = FRAME_QUEUE_CAP;
    v_queue.items = av_malloc_array(v_queue.cap, sizeof(AVFrame *));
    for (int i = 0; i < v_queue.cap; i++) v_queue.items[i] = av_frame_alloc();
    // audio queue
    a_queue.cap = FRAME_QUEUE_CAP;
    a_queue.items = av_malloc_array(a_queue.cap, sizeof(AVFrame *));
    for (int i = 0; i < a_queue.cap; i++) a_queue.items[i] = av_frame_alloc();

    start_threads(&ctx);

    // Initialize raylib
    int vid_width = ctx.v_ctx->width, vid_height = ctx.v_ctx->height;
    SetConfigFlags(FLAG_WINDOW_RESIZABLE);
    SetTraceLogLevel(LOG_WARNING);
    InitWindow(DEFAULT_WINDOW_HEIGHT * vid_width / vid_height,
               DEFAULT_WINDOW_HEIGHT, video_file);
    SetTargetFPS(120);
    InitAudioDevice();
    SetWindowMinSize(MIN_WINDOW_HEIGHT * vid_width / vid_height, MIN_WINDOW_HEIGHT);

    // Frame buffer
    Image img = {
        .width = vid_width,
        .height = vid_height,
        .mipmaps = 1,        
        .format = PIXELFORMAT_UNCOMPRESSED_R8G8B8A8,
        .data = ctx.out_frame->data[0],
    };
    Texture surface = LoadTextureFromImage(img);
    SetTextureFilter(surface, TEXTURE_FILTER_BILINEAR);

    //---Audio---
    ctx.sample_size = 32;
    AVFrame *f;

    // Because of buffer filling issues when frame size is unknown we scan the frames for a value
    ctx.a_buffer_size = ctx.a_ctx->frame_size;
    if (!ctx.a_ctx->frame_size) {
        for (int i = a_queue.rindex; i < a_queue.windex; i++) {
            f = a_queue.items[i];
            ctx.a_buffer_size = f->nb_samples > ctx.a_buffer_size ? f->nb_samples : ctx.a_buffer_size;
        }
    }
    assert(ctx.a_buffer_size != 0);
    SetAudioStreamBufferSizeDefault(ctx.a_buffer_size);
    ctx.audio_stream = LoadAudioStream(ctx.a_ctx->sample_rate, ctx.sample_size,
                                       ctx.a_ctx->ch_layout.nb_channels);
    ctx.volume = 1.0f;
    SetAudioStreamVolume(ctx.audio_stream, ctx.volume);

    int size = ctx.a_buffer_size * ctx.audio_stream.channels * (ctx.audio_stream.sampleSize / 8);
    audio_buffer = av_malloc(size);
    LOG("PLAYING...");

    main_loop(&ctx, surface);

    CloseWindow();
    CloseAudioDevice();

    return 0;
}
