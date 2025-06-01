#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>
#include <math.h>

#ifdef _WIN32
#include <windows.h>
typedef HANDLE pthread_t;
typedef HANDLE pthread_mutex_t;
typedef HANDLE pthread_cond_t;
#define THREAD_FUNC DWORD WINAPI
#define THREAD_RETURN DWORD
#else
#include <pthread.h>
#include <dlfcn.h>
#define THREAD_FUNC void*
#define THREAD_RETURN void*
#endif

#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/opt.h>
#include <libavutil/imgutils.h>
#include <libavutil/pixdesc.h>
#include <libswscale/swscale.h>
#include <libavfilter/avfilter.h>
#include <libavfilter/buffersink.h>
#include <libavfilter/buffersrc.h>
#include <libavutil/hwcontext.h>

#ifdef HAVE_VAPOURSYNTH
#include <vapoursynth/VapourSynth.h>
#include <vapoursynth/VSHelper.h>
#endif

#ifndef CLAMP
#define CLAMP(x, min, max) ((x) < (min) ? (min) : ((x) > (max) ? (max) : (x)))
#endif

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

typedef struct {
    bool blur;
    float blur_amount;
    char blur_output_fps[32];
    char blur_weighting[32];
    float* custom_weights;
    int custom_weights_count;
    bool interpolate;
    char interpolated_fps[32];
    char interpolation_method[32];
    int interpolation_block_size;
    float interpolation_mask_area;
    bool pre_interpolation;
    char pre_interpolated_fps[32];
    int quality;
    bool deduplicate;
    int deduplicate_range;
    float deduplicate_threshold;
    bool gpu_decoding;
    bool gpu_interpolation;
    bool gpu_encoding;
    char gpu_type[32];
    char input_file[512];
    char output_file[512];
    bool manual_svp;
    char svp_super_string[256];
    char svp_vectors_string[256];
    char svp_smooth_string[256];
    char svp_preset[32];
    int svp_algorithm;
    float brightness;
    float saturation;
    float contrast;
    float gamma;
    char container[32];
    char codec[32];
    int bitrate;
    char pixel_format[32];
    int threads;
    bool verbose;
    bool debug;
    float timescale;
    bool pitch_correction;
    char ffmpeg_filters[1024];
} BlurConfig;

typedef struct {
    AVFormatContext* fmt_ctx;
    AVCodecContext* codec_ctx;
    AVStream* video_stream;
    AVStream* audio_stream;
    int video_stream_idx;
    int audio_stream_idx;
    AVFrame* frame;
    AVPacket* packet;
    struct SwsContext* sws_ctx;
    AVBufferRef* hw_device_ctx;
} VideoContext;

#ifdef HAVE_VAPOURSYNTH
typedef struct {
    VSCore* core;
    VSScript* script;
    VSNodeRef* node;
    const VSAPI* vsapi;
    void* library;
} VapourSynthContext;
#endif

typedef struct {
    uint8_t** data;
    int* linesize;
    int width;
    int height;
    int64_t pts;
    int format;
    bool allocated;
} FrameBuffer;

typedef struct {
    FrameBuffer* frames;
    int capacity;
    int count;
    int read_pos;
    int write_pos;
    bool finished;
#ifdef _WIN32
    HANDLE mutex;
    HANDLE not_empty;
    HANDLE not_full;
#else
    pthread_mutex_t mutex;
    pthread_cond_t not_empty;
    pthread_cond_t not_full;
#endif
} FrameQueue;

typedef struct {
    FrameBuffer* buffer;
    int count;
    int capacity;
    int current_pos;
} BlurFrameBuffer;

static VideoContext* g_input_ctx = NULL;
static VideoContext* g_output_ctx = NULL;
static FrameQueue* g_frame_queue = NULL;
static AVFilterGraph* g_filter_graph = NULL;
static AVFilterContext* g_buffersrc_ctx = NULL;
static AVFilterContext* g_buffersink_ctx = NULL;

#ifdef HAVE_VAPOURSYNTH
static VapourSynthContext* g_vs_ctx = NULL;
#endif

extern void update_progress(int64_t frames);
extern bool is_interrupted(void);
extern float* config_get_weights(const BlurConfig* config, int frame_count, int* weight_count);

#ifdef _WIN32
static void frame_queue_init(FrameQueue* queue, int capacity) {
    queue->frames = (FrameBuffer*)calloc(capacity, sizeof(FrameBuffer));
    queue->capacity = capacity;
    queue->count = 0;
    queue->read_pos = 0;
    queue->write_pos = 0;
    queue->finished = false;
    queue->mutex = CreateMutex(NULL, FALSE, NULL);
    queue->not_empty = CreateEvent(NULL, FALSE, FALSE, NULL);
    queue->not_full = CreateEvent(NULL, FALSE, FALSE, NULL);
}

static void frame_queue_destroy(FrameQueue* queue) {
    if (!queue) return;

    for (int i = 0; i < queue->capacity; i++) {
        if (queue->frames[i].allocated && queue->frames[i].data) {
            for (int j = 0; j < 4; j++) {
                if (queue->frames[i].data[j]) {
                    av_free(queue->frames[i].data[j]);
                }
            }
            av_free(queue->frames[i].data);
            av_free(queue->frames[i].linesize);
        }
    }

    free(queue->frames);
    CloseHandle(queue->mutex);
    CloseHandle(queue->not_empty);
    CloseHandle(queue->not_full);
}

static void frame_queue_signal_finished(FrameQueue* queue) {
    WaitForSingleObject(queue->mutex, INFINITE);
    queue->finished = true;
    SetEvent(queue->not_empty);
    ReleaseMutex(queue->mutex);
}
#else
static void frame_queue_init(FrameQueue* queue, int capacity) {
    queue->frames = (FrameBuffer*)calloc(capacity, sizeof(FrameBuffer));
    queue->capacity = capacity;
    queue->count = 0;
    queue->read_pos = 0;
    queue->write_pos = 0;
    queue->finished = false;
    pthread_mutex_init(&queue->mutex, NULL);
    pthread_cond_init(&queue->not_empty, NULL);
    pthread_cond_init(&queue->not_full, NULL);
}

static void frame_queue_destroy(FrameQueue* queue) {
    if (!queue) return;

    for (int i = 0; i < queue->capacity; i++) {
        if (queue->frames[i].allocated && queue->frames[i].data) {
            for (int j = 0; j < 4; j++) {
                if (queue->frames[i].data[j]) {
                    av_free(queue->frames[i].data[j]);
                }
            }
            av_free(queue->frames[i].data);
            av_free(queue->frames[i].linesize);
        }
    }

    free(queue->frames);
    pthread_mutex_destroy(&queue->mutex);
    pthread_cond_destroy(&queue->not_empty);
    pthread_cond_destroy(&queue->not_full);
}

static void frame_queue_signal_finished(FrameQueue* queue) {
    pthread_mutex_lock(&queue->mutex);
    queue->finished = true;
    pthread_cond_broadcast(&queue->not_empty);
    pthread_mutex_unlock(&queue->mutex);
}
#endif

static bool frame_buffer_alloc(FrameBuffer* buffer, int width, int height, int format) {
    buffer->width = width;
    buffer->height = height;
    buffer->format = format;
    buffer->data = (uint8_t**)av_malloc(4 * sizeof(uint8_t*));
    buffer->linesize = (int*)av_malloc(4 * sizeof(int));

    if (!buffer->data || !buffer->linesize) {
        return false;
    }

    int ret = av_image_alloc(buffer->data, buffer->linesize, width, height, format, 32);
    if (ret < 0) {
        av_free(buffer->data);
        av_free(buffer->linesize);
        return false;
    }

    buffer->allocated = true;
    return true;
}

static void frame_buffer_copy(FrameBuffer* dst, AVFrame* src) {
    if (!dst->allocated) {
        frame_buffer_alloc(dst, src->width, src->height, src->format);
    }

    av_image_copy(dst->data, dst->linesize,
        (const uint8_t**)src->data, src->linesize,
        src->format, src->width, src->height);
    dst->pts = src->pts;
}

static void frame_buffer_to_avframe(FrameBuffer* buffer, AVFrame* frame) {
    for (int i = 0; i < 4; i++) {
        frame->data[i] = buffer->data[i];
        frame->linesize[i] = buffer->linesize[i];
    }
    frame->width = buffer->width;
    frame->height = buffer->height;
    frame->format = buffer->format;
    frame->pts = buffer->pts;
}

static bool frame_queue_push(FrameQueue* queue, AVFrame* frame) {
#ifdef _WIN32
    WaitForSingleObject(queue->mutex, INFINITE);

    while (queue->count >= queue->capacity && !is_interrupted()) {
        ReleaseMutex(queue->mutex);
        WaitForSingleObject(queue->not_full, 1000);
        WaitForSingleObject(queue->mutex, INFINITE);
    }

    if (is_interrupted()) {
        ReleaseMutex(queue->mutex);
        return false;
    }
#else
    pthread_mutex_lock(&queue->mutex);

    while (queue->count >= queue->capacity && !is_interrupted()) {
        struct timespec timeout;
        clock_gettime(CLOCK_REALTIME, &timeout);
        timeout.tv_sec += 1;
        pthread_cond_timedwait(&queue->not_full, &queue->mutex, &timeout);
    }

    if (is_interrupted()) {
        pthread_mutex_unlock(&queue->mutex);
        return false;
    }
#endif

    FrameBuffer* buffer = &queue->frames[queue->write_pos];
    frame_buffer_copy(buffer, frame);

    queue->write_pos = (queue->write_pos + 1) % queue->capacity;
    queue->count++;

#ifdef _WIN32
    SetEvent(queue->not_empty);
    ReleaseMutex(queue->mutex);
#else
    pthread_cond_signal(&queue->not_empty);
    pthread_mutex_unlock(&queue->mutex);
#endif

    return true;
}

static bool frame_queue_pop(FrameQueue* queue, AVFrame* frame) {
#ifdef _WIN32
    WaitForSingleObject(queue->mutex, INFINITE);

    while (queue->count == 0 && !queue->finished && !is_interrupted()) {
        ReleaseMutex(queue->mutex);
        WaitForSingleObject(queue->not_empty, 1000);
        WaitForSingleObject(queue->mutex, INFINITE);
    }

    if ((queue->count == 0 && queue->finished) || is_interrupted()) {
        ReleaseMutex(queue->mutex);
        return false;
    }
#else
    pthread_mutex_lock(&queue->mutex);

    while (queue->count == 0 && !queue->finished && !is_interrupted()) {
        struct timespec timeout;
        clock_gettime(CLOCK_REALTIME, &timeout);
        timeout.tv_sec += 1;
        pthread_cond_timedwait(&queue->not_empty, &queue->mutex, &timeout);
    }

    if ((queue->count == 0 && queue->finished) || is_interrupted()) {
        pthread_mutex_unlock(&queue->mutex);
        return false;
    }
#endif

    FrameBuffer* buffer = &queue->frames[queue->read_pos];
    frame_buffer_to_avframe(buffer, frame);

    queue->read_pos = (queue->read_pos + 1) % queue->capacity;
    queue->count--;

#ifdef _WIN32
    SetEvent(queue->not_full);
    ReleaseMutex(queue->mutex);
#else
    pthread_cond_signal(&queue->not_full);
    pthread_mutex_unlock(&queue->mutex);
#endif

    return true;
}

static const char* get_hw_codec_name(const char* codec, const char* gpu_type, bool encoding) {
    if (strcmp(codec, "h264") == 0) {
        if (strcmp(gpu_type, "nvidia") == 0) {
            return encoding ? "h264_nvenc" : "h264_cuvid";
        }
        else if (strcmp(gpu_type, "amd") == 0) {
            return encoding ? "h264_amf" : "h264";
        }
        else if (strcmp(gpu_type, "intel") == 0) {
            return encoding ? "h264_qsv" : "h264_qsv";
        }
    }
    else if (strcmp(codec, "h265") == 0 || strcmp(codec, "hevc") == 0) {
        if (strcmp(gpu_type, "nvidia") == 0) {
            return encoding ? "hevc_nvenc" : "hevc_cuvid";
        }
        else if (strcmp(gpu_type, "amd") == 0) {
            return encoding ? "hevc_amf" : "hevc";
        }
        else if (strcmp(gpu_type, "intel") == 0) {
            return encoding ? "hevc_qsv" : "hevc_qsv";
        }
    }
    else if (strcmp(codec, "av1") == 0) {
        if (strcmp(gpu_type, "nvidia") == 0) {
            return encoding ? "av1_nvenc" : "av1";
        }
        else if (strcmp(gpu_type, "amd") == 0) {
            return encoding ? "av1_amf" : "av1";
        }
        else if (strcmp(gpu_type, "intel") == 0) {
            return encoding ? "av1_qsv" : "av1";
        }
    }
    return codec;
}

static enum AVHWDeviceType get_hw_device_type(const char* gpu_type) {
    if (strcmp(gpu_type, "nvidia") == 0) {
        return AV_HWDEVICE_TYPE_CUDA;
    }
    else if (strcmp(gpu_type, "amd") == 0) {
#ifdef _WIN32
        return AV_HWDEVICE_TYPE_D3D11VA;
#else
        return AV_HWDEVICE_TYPE_VAAPI;
#endif
    }
    else if (strcmp(gpu_type, "intel") == 0) {
        return AV_HWDEVICE_TYPE_QSV;
    }
    return AV_HWDEVICE_TYPE_NONE;
}

static enum AVPixelFormat get_pixel_format(const char* format_name) {
    if (strcmp(format_name, "yuv420p") == 0) return AV_PIX_FMT_YUV420P;
    if (strcmp(format_name, "yuv422p") == 0) return AV_PIX_FMT_YUV422P;
    if (strcmp(format_name, "yuv444p") == 0) return AV_PIX_FMT_YUV444P;
    if (strcmp(format_name, "yuv420p10le") == 0) return AV_PIX_FMT_YUV420P10LE;
    if (strcmp(format_name, "yuv422p10le") == 0) return AV_PIX_FMT_YUV422P10LE;
    if (strcmp(format_name, "yuv444p10le") == 0) return AV_PIX_FMT_YUV444P10LE;
    return AV_PIX_FMT_YUV420P;
}

static bool open_input_video(VideoContext* ctx, const char* filename, const BlurConfig* config) {
    int ret;

    ctx->fmt_ctx = NULL;
    ret = avformat_open_input(&ctx->fmt_ctx, filename, NULL, NULL);
    if (ret < 0) {
        char errbuf[AV_ERROR_MAX_STRING_SIZE];
        av_strerror(ret, errbuf, AV_ERROR_MAX_STRING_SIZE);
        fprintf(stderr, "Error opening input file '%s': %s\n", filename, errbuf);
        return false;
    }

    ret = avformat_find_stream_info(ctx->fmt_ctx, NULL);
    if (ret < 0) {
        fprintf(stderr, "Error finding stream information\n");
        return false;
    }

    ctx->video_stream_idx = -1;
    ctx->audio_stream_idx = -1;

    for (unsigned int i = 0; i < ctx->fmt_ctx->nb_streams; i++) {
        if (ctx->fmt_ctx->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_VIDEO &&
            ctx->video_stream_idx == -1) {
            ctx->video_stream_idx = i;
            ctx->video_stream = ctx->fmt_ctx->streams[i];
        }
        else if (ctx->fmt_ctx->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_AUDIO &&
            ctx->audio_stream_idx == -1) {
            ctx->audio_stream_idx = i;
            ctx->audio_stream = ctx->fmt_ctx->streams[i];
        }
    }

    if (ctx->video_stream_idx == -1) {
        fprintf(stderr, "No video stream found in input file\n");
        return false;
    }

    const char* codec_name = NULL;
    if (config->gpu_decoding) {
        enum AVHWDeviceType hw_type = get_hw_device_type(config->gpu_type);
        if (hw_type != AV_HWDEVICE_TYPE_NONE) {
            ret = av_hwdevice_ctx_create(&ctx->hw_device_ctx, hw_type, NULL, NULL, 0);
            if (ret < 0) {
                if (config->verbose) {
                    fprintf(stderr, "Warning: Failed to create hardware device context, using software decoding\n");
                }
                ctx->hw_device_ctx = NULL;
            }
            else {
                codec_name = get_hw_codec_name(
                    avcodec_get_name(ctx->video_stream->codecpar->codec_id),
                    config->gpu_type, false);
                if (config->verbose) {
                    printf("Using hardware decoder: %s\n", codec_name);
                }
            }
        }
    }

    const AVCodec* codec = codec_name ?
        avcodec_find_decoder_by_name(codec_name) :
        avcodec_find_decoder(ctx->video_stream->codecpar->codec_id);

    if (!codec) {
        fprintf(stderr, "Codec not found for stream\n");
        return false;
    }

    ctx->codec_ctx = avcodec_alloc_context3(codec);
    if (!ctx->codec_ctx) {
        fprintf(stderr, "Failed to allocate codec context\n");
        return false;
    }

    ret = avcodec_parameters_to_context(ctx->codec_ctx, ctx->video_stream->codecpar);
    if (ret < 0) {
        fprintf(stderr, "Failed to copy codec parameters to decoder context\n");
        return false;
    }

    if (ctx->hw_device_ctx) {
        ctx->codec_ctx->hw_device_ctx = av_buffer_ref(ctx->hw_device_ctx);
    }

    if (config->threads > 0) {
        ctx->codec_ctx->thread_count = config->threads;
        ctx->codec_ctx->thread_type = FF_THREAD_FRAME | FF_THREAD_SLICE;
    }

    ret = avcodec_open2(ctx->codec_ctx, codec, NULL);
    if (ret < 0) {
        fprintf(stderr, "Failed to open codec\n");
        return false;
    }

    ctx->frame = av_frame_alloc();
    ctx->packet = av_packet_alloc();

    if (!ctx->frame || !ctx->packet) {
        fprintf(stderr, "Failed to allocate frame or packet\n");
        return false;
    }

    return true;
}

static bool create_output_video(VideoContext* ctx, const char* filename, const BlurConfig* config,
    int width, int height, double fps) {
    int ret;

    const char* format_name = NULL;
    if (strstr(filename, ".mp4")) format_name = "mp4";
    else if (strstr(filename, ".mkv")) format_name = "matroska";
    else if (strstr(filename, ".avi")) format_name = "avi";
    else if (strstr(filename, ".mov")) format_name = "mov";

    ret = avformat_alloc_output_context2(&ctx->fmt_ctx, NULL, format_name, filename);
    if (!ctx->fmt_ctx) {
        fprintf(stderr, "Failed to create output format context\n");
        return false;
    }

    const char* codec_name = config->codec;
    if (config->gpu_encoding) {
        codec_name = get_hw_codec_name(config->codec, config->gpu_type, true);
        if (config->verbose) {
            printf("Using hardware encoder: %s\n", codec_name);
        }
    }

    const AVCodec* codec = avcodec_find_encoder_by_name(codec_name);
    if (!codec) {
        codec = avcodec_find_encoder_by_name(config->codec);
        if (!codec) {
            codec = avcodec_find_encoder(AV_CODEC_ID_H264);
        }
    }

    if (!codec) {
        fprintf(stderr, "Encoder not found\n");
        return false;
    }

    ctx->video_stream = avformat_new_stream(ctx->fmt_ctx, codec);
    if (!ctx->video_stream) {
        fprintf(stderr, "Failed to create output video stream\n");
        return false;
    }

    ctx->codec_ctx = avcodec_alloc_context3(codec);
    if (!ctx->codec_ctx) {
        fprintf(stderr, "Failed to allocate encoder context\n");
        return false;
    }

    ctx->codec_ctx->width = width;
    ctx->codec_ctx->height = height;
    ctx->codec_ctx->time_base = av_d2q(1.0 / fps, 1000000);
    ctx->codec_ctx->framerate = av_d2q(fps, 1000000);
    ctx->codec_ctx->gop_size = (int)(fps * 2);
    ctx->codec_ctx->max_b_frames = 2;
    ctx->codec_ctx->pix_fmt = get_pixel_format(config->pixel_format);

    if (config->gpu_encoding && g_input_ctx && g_input_ctx->hw_device_ctx) {
        ctx->codec_ctx->hw_device_ctx = av_buffer_ref(g_input_ctx->hw_device_ctx);
    }

    if (config->bitrate > 0) {
        ctx->codec_ctx->bit_rate = config->bitrate * 1000;
        ctx->codec_ctx->rc_max_rate = config->bitrate * 1200;
        ctx->codec_ctx->rc_buffer_size = config->bitrate * 2000;
    }
    else {
        if (codec->id == AV_CODEC_ID_H264 || codec->id == AV_CODEC_ID_HEVC) {
            av_opt_set_int(ctx->codec_ctx->priv_data, "crf", config->quality, 0);
        }
        else {
            ctx->codec_ctx->qmin = config->quality;
            ctx->codec_ctx->qmax = config->quality + 10;
        }
    }

    if (config->threads > 0) {
        ctx->codec_ctx->thread_count = config->threads;
    }

    if (ctx->fmt_ctx->oformat->flags & AVFMT_GLOBALHEADER) {
        ctx->codec_ctx->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;
    }

    if (config->gpu_encoding) {
        av_opt_set(ctx->codec_ctx->priv_data, "preset", "fast", 0);
        av_opt_set(ctx->codec_ctx->priv_data, "tune", "zerolatency", 0);
    }

    ret = avcodec_open2(ctx->codec_ctx, codec, NULL);
    if (ret < 0) {
        char errbuf[AV_ERROR_MAX_STRING_SIZE];
        av_strerror(ret, errbuf, AV_ERROR_MAX_STRING_SIZE);
        fprintf(stderr, "Failed to open encoder: %s\n", errbuf);
        return false;
    }

    ret = avcodec_parameters_from_context(ctx->video_stream->codecpar, ctx->codec_ctx);
    if (ret < 0) {
        fprintf(stderr, "Failed to copy encoder parameters to stream\n");
        return false;
    }

    ctx->video_stream->time_base = ctx->codec_ctx->time_base;

    if (g_input_ctx && g_input_ctx->audio_stream_idx >= 0) {
        AVStream* in_stream = g_input_ctx->fmt_ctx->streams[g_input_ctx->audio_stream_idx];
        AVStream* out_stream = avformat_new_stream(ctx->fmt_ctx, NULL);

        if (out_stream) {
            ret = avcodec_parameters_copy(out_stream->codecpar, in_stream->codecpar);
            if (ret >= 0) {
                out_stream->time_base = in_stream->time_base;
                if (config->timescale != 1.0) {
                    out_stream->time_base.den = (int)(out_stream->time_base.den * config->timescale);
                }
                ctx->audio_stream = out_stream;
                ctx->audio_stream_idx = out_stream->index;
                if (config->verbose) {
                    printf("Copying audio stream\n");
                }
            }
        }
    }

    ret = avio_open(&ctx->fmt_ctx->pb, filename, AVIO_FLAG_WRITE);
    if (ret < 0) {
        char errbuf[AV_ERROR_MAX_STRING_SIZE];
        av_strerror(ret, errbuf, AV_ERROR_MAX_STRING_SIZE);
        fprintf(stderr, "Failed to open output file '%s': %s\n", filename, errbuf);
        return false;
    }

    ret = avformat_write_header(ctx->fmt_ctx, NULL);
    if (ret < 0) {
        char errbuf[AV_ERROR_MAX_STRING_SIZE];
        av_strerror(ret, errbuf, AV_ERROR_MAX_STRING_SIZE);
        fprintf(stderr, "Failed to write output header: %s\n", errbuf);
        return false;
    }

    ctx->frame = av_frame_alloc();
    ctx->packet = av_packet_alloc();

    return true;
}

#ifdef HAVE_VAPOURSYNTH
static bool init_vapoursynth(VapourSynthContext* vs_ctx) {
#ifdef _WIN32
    vs_ctx->library = LoadLibrary(L"vapoursynth.dll");
#else
    vs_ctx->library = dlopen("libvapoursynth.so", RTLD_LAZY);
#endif

    if (!vs_ctx->library) {
        return false;
    }

    typedef const VSAPI* (*VSGetAPIFunc)(int version);
    VSGetAPIFunc getVapourSynthAPI;

#ifdef _WIN32
    getVapourSynthAPI = (VSGetAPIFunc)GetProcAddress(vs_ctx->library, "getVapourSynthAPI");
#else
    getVapourSynthAPI = (VSGetAPIFunc)dlsym(vs_ctx->library, "getVapourSynthAPI");
#endif

    if (!getVapourSynthAPI) {
        return false;
    }

    vs_ctx->vsapi = getVapourSynthAPI(VAPOURSYNTH_API_VERSION);
    if (!vs_ctx->vsapi) {
        return false;
    }

    vs_ctx->core = vs_ctx->vsapi->createCore(0);
    if (!vs_ctx->core) {
        return false;
    }

    return true;
}
#endif

static bool create_filter_graph(const BlurConfig* config, int width, int height, double fps) {
    int ret;
    char args[512];

    g_filter_graph = avfilter_graph_alloc();
    if (!g_filter_graph) {
        return false;
    }

    if (config->threads > 0) {
        g_filter_graph->nb_threads = config->threads;
    }

    snprintf(args, sizeof(args),
        "video_size=%dx%d:pix_fmt=%d:time_base=%d/%d:pixel_aspect=1/1",
        width, height, AV_PIX_FMT_YUV420P, 1, (int)fps);

    const AVFilter* buffersrc = avfilter_get_by_name("buffer");
    ret = avfilter_graph_create_filter(&g_buffersrc_ctx, buffersrc, "in",
        args, NULL, g_filter_graph);
    if (ret < 0) {
        fprintf(stderr, "Failed to create buffer source filter\n");
        return false;
    }

    const AVFilter* buffersink = avfilter_get_by_name("buffersink");
    ret = avfilter_graph_create_filter(&g_buffersink_ctx, buffersink, "out",
        NULL, NULL, g_filter_graph);
    if (ret < 0) {
        fprintf(stderr, "Failed to create buffer sink filter\n");
        return false;
    }

    enum AVPixelFormat pix_fmts[] = { AV_PIX_FMT_YUV420P, AV_PIX_FMT_NONE };
    ret = av_opt_set_int_list(g_buffersink_ctx, "pix_fmts", pix_fmts,
        AV_PIX_FMT_NONE, AV_OPT_SEARCH_CHILDREN);
    if (ret < 0) {
        fprintf(stderr, "Failed to set pixel formats on buffer sink\n");
        return false;
    }

    AVFilterInOut* outputs = avfilter_inout_alloc();
    AVFilterInOut* inputs = avfilter_inout_alloc();

    outputs->name = av_strdup("in");
    outputs->filter_ctx = g_buffersrc_ctx;
    outputs->pad_idx = 0;
    outputs->next = NULL;

    inputs->name = av_strdup("out");
    inputs->filter_ctx = g_buffersink_ctx;
    inputs->pad_idx = 0;
    inputs->next = NULL;

    char filter_descr[2048] = "null";

    if (strlen(config->ffmpeg_filters) > 0) {
        strncpy(filter_descr, config->ffmpeg_filters, sizeof(filter_descr) - 1);
    }
    else if (config->brightness != 0 || config->saturation != 0 ||
        config->contrast != 0 || config->gamma != 1.0) {

        float bright = config->brightness;
        float sat = 1.0f + config->saturation;
        float cont = 1.0f + config->contrast;
        float gamma = config->gamma;

        snprintf(filter_descr, sizeof(filter_descr),
            "eq=brightness=%.2f:saturation=%.2f:contrast=%.2f:gamma=%.2f",
            bright, sat, cont, gamma);
    }

    if (config->debug) {
        printf("Filter description: %s\n", filter_descr);
    }

    ret = avfilter_graph_parse_ptr(g_filter_graph, filter_descr, &inputs, &outputs, NULL);
    if (ret < 0) {
        fprintf(stderr, "Failed to parse filter graph\n");
        avfilter_inout_free(&inputs);
        avfilter_inout_free(&outputs);
        return false;
    }

    ret = avfilter_graph_config(g_filter_graph, NULL);
    if (ret < 0) {
        fprintf(stderr, "Failed to configure filter graph\n");
        return false;
    }

    avfilter_inout_free(&inputs);
    avfilter_inout_free(&outputs);

    return true;
}

static bool apply_motion_blur(FrameBuffer* frames, int frame_count, float* weights, FrameBuffer* output) {
    if (frame_count == 0 || !frames || !weights || !output) return false;

    int width = frames[0].width;
    int height = frames[0].height;

    if (!output->allocated) {
        if (!frame_buffer_alloc(output, width, height, AV_PIX_FMT_YUV420P)) {
            return false;
        }
    }

    memset(output->data[0], 0, output->linesize[0] * height);
    memset(output->data[1], 0, output->linesize[1] * (height / 2));
    memset(output->data[2], 0, output->linesize[2] * (height / 2));

    for (int y = 0; y < height; y++) {
        for (int x = 0; x < width; x++) {
            float y_accum = 0, u_accum = 0, v_accum = 0;

            for (int i = 0; i < frame_count; i++) {
                if (!frames[i].data[0]) continue;

                uint8_t y_val = frames[i].data[0][y * frames[i].linesize[0] + x];
                y_accum += y_val * weights[i];

                if (y % 2 == 0 && x % 2 == 0) {
                    uint8_t u_val = frames[i].data[1][(y / 2) * frames[i].linesize[1] + (x / 2)];
                    uint8_t v_val = frames[i].data[2][(y / 2) * frames[i].linesize[2] + (x / 2)];
                    u_accum += u_val * weights[i];
                    v_accum += v_val * weights[i];
                }
            }

            output->data[0][y * output->linesize[0] + x] = (uint8_t)CLAMP(y_accum, 0, 255);

            if (y % 2 == 0 && x % 2 == 0) {
                output->data[1][(y / 2) * output->linesize[1] + (x / 2)] = (uint8_t)CLAMP(u_accum, 0, 255);
                output->data[2][(y / 2) * output->linesize[2] + (x / 2)] = (uint8_t)CLAMP(v_accum, 0, 255);
            }
        }
    }

    output->pts = frames[frame_count / 2].pts;
    return true;
}

static bool detect_duplicate_frames(FrameBuffer* frame1, FrameBuffer* frame2, float threshold) {
    if (!frame1 || !frame2 || !frame1->data[0] || !frame2->data[0]) return false;

    int width = frame1->width;
    int height = frame1->height;

    if (width != frame2->width || height != frame2->height) return false;

    int64_t diff_sum = 0;
    int total_pixels = width * height;

    for (int y = 0; y < height; y++) {
        for (int x = 0; x < width; x++) {
            int val1 = frame1->data[0][y * frame1->linesize[0] + x];
            int val2 = frame2->data[0][y * frame2->linesize[0] + x];
            diff_sum += abs(val1 - val2);
        }
    }

    float avg_diff = (float)diff_sum / total_pixels;
    return avg_diff < (threshold * 255.0f);
}

static double parse_fps_string(const char* fps_str, double base_fps) {
    if (strstr(fps_str, "x")) {
        double multiplier = atof(fps_str);
        return base_fps * multiplier;
    }
    return atof(fps_str);
}

static THREAD_FUNC processing_thread(void* arg) {
    BlurConfig* config = (BlurConfig*)arg;
    BlurFrameBuffer blur_buffer = { 0 };
    int weight_count = 0;
    float* weights = NULL;

    double input_fps = av_q2d(g_input_ctx->video_stream->avg_frame_rate);
    double output_fps = parse_fps_string(config->blur_output_fps, input_fps);

    int blur_frame_count = (int)(output_fps / input_fps * config->blur_amount * 5.0 + 0.5);
    if (blur_frame_count < 1) blur_frame_count = 1;
    if (blur_frame_count > 64) blur_frame_count = 64;

    weights = config_get_weights(config, blur_frame_count, &weight_count);
    if (!weights) {
        fprintf(stderr, "Failed to generate blur weights\n");
#ifdef _WIN32
        return 1;
#else
        return NULL;
#endif
    }

    blur_buffer.capacity = blur_frame_count;
    blur_buffer.buffer = (FrameBuffer*)calloc(blur_frame_count, sizeof(FrameBuffer));
    blur_buffer.count = 0;
    blur_buffer.current_pos = 0;

    if (!blur_buffer.buffer) {
        free(weights);
#ifdef _WIN32
        return 1;
#else
        return NULL;
#endif
    }

    FrameBuffer output_buffer = { 0 };
    AVFrame* output_frame = av_frame_alloc();
    AVFrame* input_frame = av_frame_alloc();

    int frames_processed = 0;
    FrameBuffer dedup_frames[16] = { 0 };
    int dedup_count = 0;

    if (config->verbose) {
        printf("Processing with %d blur frames, weights: ", blur_frame_count);
        for (int i = 0; i < weight_count; i++) {
            printf("%.3f ", weights[i]);
        }
        printf("\n");
    }

    while (!is_interrupted()) {
        if (!frame_queue_pop(g_frame_queue, input_frame)) {
            break;
        }

        bool is_duplicate = false;

        if (config->deduplicate && dedup_count > 0) {
            FrameBuffer temp_buffer = { 0 };
            frame_buffer_copy(&temp_buffer, input_frame);

            for (int i = 0; i < dedup_count && i < config->deduplicate_range; i++) {
                if (detect_duplicate_frames(&temp_buffer, &dedup_frames[i], config->deduplicate_threshold)) {
                    is_duplicate = true;
                    break;
                }
            }

            if (!is_duplicate && dedup_count < 16) {
                if (dedup_frames[dedup_count].allocated) {
                    for (int j = 0; j < 4; j++) {
                        if (dedup_frames[dedup_count].data[j]) {
                            av_free(dedup_frames[dedup_count].data[j]);
                        }
                    }
                    av_free(dedup_frames[dedup_count].data);
                    av_free(dedup_frames[dedup_count].linesize);
                }
                dedup_frames[dedup_count] = temp_buffer;
                dedup_count++;
            }
        }

        if (is_duplicate) {
            av_frame_unref(input_frame);
            continue;
        }

        FrameBuffer* current_buffer = &blur_buffer.buffer[blur_buffer.current_pos];
        frame_buffer_copy(current_buffer, input_frame);

        blur_buffer.current_pos = (blur_buffer.current_pos + 1) % blur_buffer.capacity;
        if (blur_buffer.count < blur_buffer.capacity) {
            blur_buffer.count++;
        }

        if (blur_buffer.count >= blur_frame_count) {
            FrameBuffer* ordered_frames = (FrameBuffer*)malloc(blur_frame_count * sizeof(FrameBuffer));

            for (int i = 0; i < blur_frame_count; i++) {
                int idx = (blur_buffer.current_pos - blur_frame_count + i + blur_buffer.capacity) % blur_buffer.capacity;
                ordered_frames[i] = blur_buffer.buffer[idx];
            }

            if (apply_motion_blur(ordered_frames, blur_frame_count, weights, &output_buffer)) {
                frame_buffer_to_avframe(&output_buffer, output_frame);

                int ret = avcodec_send_frame(g_output_ctx->codec_ctx, output_frame);
                if (ret < 0) {
                    if (config->debug) {
                        fprintf(stderr, "Error sending frame to encoder: %d\n", ret);
                    }
                }

                while (ret >= 0) {
                    ret = avcodec_receive_packet(g_output_ctx->codec_ctx, g_output_ctx->packet);
                    if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF) {
                        break;
                    }
                    else if (ret < 0) {
                        if (config->debug) {
                            fprintf(stderr, "Error receiving packet from encoder: %d\n", ret);
                        }
                        break;
                    }

                    g_output_ctx->packet->stream_index = g_output_ctx->video_stream->index;
                    av_packet_rescale_ts(g_output_ctx->packet,
                        g_output_ctx->codec_ctx->time_base,
                        g_output_ctx->video_stream->time_base);

                    ret = av_interleaved_write_frame(g_output_ctx->fmt_ctx, g_output_ctx->packet);
                    if (ret < 0) {
                        if (config->debug) {
                            fprintf(stderr, "Error writing packet: %d\n", ret);
                        }
                    }

                    av_packet_unref(g_output_ctx->packet);
                }
            }

            free(ordered_frames);
        }

        frames_processed++;
        if (frames_processed % 30 == 0) {
            update_progress(frames_processed);
        }

        av_frame_unref(input_frame);
    }

    avcodec_send_frame(g_output_ctx->codec_ctx, NULL);
    while (true) {
        int ret = avcodec_receive_packet(g_output_ctx->codec_ctx, g_output_ctx->packet);
        if (ret == AVERROR_EOF) {
            break;
        }
        else if (ret < 0) {
            break;
        }

        g_output_ctx->packet->stream_index = g_output_ctx->video_stream->index;
        av_packet_rescale_ts(g_output_ctx->packet,
            g_output_ctx->codec_ctx->time_base,
            g_output_ctx->video_stream->time_base);

        av_interleaved_write_frame(g_output_ctx->fmt_ctx, g_output_ctx->packet);
        av_packet_unref(g_output_ctx->packet);
    }

    if (output_buffer.allocated && output_buffer.data) {
        for (int i = 0; i < 4; i++) {
            if (output_buffer.data[i]) {
                av_free(output_buffer.data[i]);
            }
        }
        av_free(output_buffer.data);
        av_free(output_buffer.linesize);
    }

    for (int i = 0; i < blur_buffer.capacity; i++) {
        if (blur_buffer.buffer[i].allocated && blur_buffer.buffer[i].data) {
            for (int j = 0; j < 4; j++) {
                if (blur_buffer.buffer[i].data[j]) {
                    av_free(blur_buffer.buffer[i].data[j]);
                }
            }
            av_free(blur_buffer.buffer[i].data);
            av_free(blur_buffer.buffer[i].linesize);
        }
    }

    for (int i = 0; i < dedup_count; i++) {
        if (dedup_frames[i].allocated && dedup_frames[i].data) {
            for (int j = 0; j < 4; j++) {
                if (dedup_frames[i].data[j]) {
                    av_free(dedup_frames[i].data[j]);
                }
            }
            av_free(dedup_frames[i].data);
            av_free(dedup_frames[i].linesize);
        }
    }

    free(blur_buffer.buffer);
    free(weights);
    av_frame_free(&output_frame);
    av_frame_free(&input_frame);

    if (config->verbose) {
        printf("Processing thread finished, processed %d frames\n", frames_processed);
    }

#ifdef _WIN32
    return 0;
#else
    return NULL;
#endif
}

bool video_process(const BlurConfig* config) {
    int width = g_input_ctx->codec_ctx->width;
    int height = g_input_ctx->codec_ctx->height;
    double input_fps = av_q2d(g_input_ctx->video_stream->avg_frame_rate);
    double output_fps = parse_fps_string(config->blur_output_fps, input_fps);

    if (config->timescale != 1.0) {
        output_fps *= config->timescale;
    }

    printf("Processing %dx%d video: %.2f fps -> %.2f fps\n", width, height, input_fps, output_fps);

    g_output_ctx = (VideoContext*)calloc(1, sizeof(VideoContext));
    if (!g_output_ctx) {
        fprintf(stderr, "Failed to allocate output context\n");
        return false;
    }

    if (config->gpu_encoding && g_input_ctx->hw_device_ctx) {
        g_output_ctx->hw_device_ctx = g_input_ctx->hw_device_ctx;
    }

    if (!create_output_video(g_output_ctx, config->output_file, config, width, height, output_fps)) {
        return false;
    }

    if (strlen(config->ffmpeg_filters) > 0 || config->brightness != 0 ||
        config->saturation != 0 || config->contrast != 0 || config->gamma != 1.0) {
        if (!create_filter_graph(config, width, height, input_fps)) {
            fprintf(stderr, "Warning: Filter graph creation failed, continuing without filters\n");
        }
    }

    g_frame_queue = (FrameQueue*)calloc(1, sizeof(FrameQueue));
    if (!g_frame_queue) {
        fprintf(stderr, "Failed to allocate frame queue\n");
        return false;
    }

    frame_queue_init(g_frame_queue, 200);

#ifdef _WIN32
    HANDLE processing_tid = CreateThread(NULL, 0, processing_thread, (void*)config, 0, NULL);
    if (!processing_tid) {
        fprintf(stderr, "Failed to create processing thread\n");
        return false;
    }
#else
    pthread_t processing_tid;
    if (pthread_create(&processing_tid, NULL, processing_thread, (void*)config) != 0) {
        fprintf(stderr, "Failed to create processing thread\n");
        return false;
    }
#endif

    AVFrame* decoded_frame = av_frame_alloc();
    AVFrame* filtered_frame = av_frame_alloc();
    int64_t frames_read = 0;
    int ret;

    if (config->verbose) {
        printf("Starting frame reading and decoding...\n");
    }

    while (!is_interrupted()) {
        ret = av_read_frame(g_input_ctx->fmt_ctx, g_input_ctx->packet);
        if (ret < 0) {
            if (ret == AVERROR_EOF) {
                if (config->verbose) {
                    printf("Reached end of input file\n");
                }
            }
            else {
                fprintf(stderr, "Error reading frame: %d\n", ret);
            }
            break;
        }

        if (g_input_ctx->packet->stream_index == g_input_ctx->video_stream_idx) {
            ret = avcodec_send_packet(g_input_ctx->codec_ctx, g_input_ctx->packet);
            if (ret < 0) {
                if (config->debug) {
                    fprintf(stderr, "Error sending packet to decoder: %d\n", ret);
                }
                av_packet_unref(g_input_ctx->packet);
                continue;
            }

            while (ret >= 0) {
                ret = avcodec_receive_frame(g_input_ctx->codec_ctx, decoded_frame);
                if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF) {
                    break;
                }
                else if (ret < 0) {
                    if (config->debug) {
                        fprintf(stderr, "Error receiving frame from decoder: %d\n", ret);
                    }
                    break;
                }

                if (g_filter_graph) {
                    ret = av_buffersrc_add_frame_flags(g_buffersrc_ctx, decoded_frame, AV_BUFFERSRC_FLAG_KEEP_REF);
                    if (ret < 0) {
                        if (config->debug) {
                            fprintf(stderr, "Error feeding frame to filter graph: %d\n", ret);
                        }
                        continue;
                    }

                    while (true) {
                        ret = av_buffersink_get_frame(g_buffersink_ctx, filtered_frame);
                        if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF) {
                            break;
                        }
                        else if (ret < 0) {
                            if (config->debug) {
                                fprintf(stderr, "Error getting frame from filter: %d\n", ret);
                            }
                            break;
                        }

                        if (!frame_queue_push(g_frame_queue, filtered_frame)) {
                            av_frame_unref(filtered_frame);
                            break;
                        }
                        av_frame_unref(filtered_frame);
                    }
                }
                else {
                    if (!frame_queue_push(g_frame_queue, decoded_frame)) {
                        break;
                    }
                }

                frames_read++;
                if (frames_read % 100 == 0 && config->verbose) {
                    printf("Read %lld frames\n", (long long)frames_read);
                }

                av_frame_unref(decoded_frame);
            }
        }
        else if (g_output_ctx->audio_stream_idx >= 0 &&
            g_input_ctx->packet->stream_index == g_input_ctx->audio_stream_idx) {

            AVPacket* audio_pkt = av_packet_clone(g_input_ctx->packet);
            if (audio_pkt) {
                audio_pkt->stream_index = g_output_ctx->audio_stream_idx;
                av_packet_rescale_ts(audio_pkt,
                    g_input_ctx->audio_stream->time_base,
                    g_output_ctx->audio_stream->time_base);

                if (config->timescale != 1.0 && !config->pitch_correction) {
                    audio_pkt->pts = (int64_t)(audio_pkt->pts / config->timescale);
                    audio_pkt->dts = (int64_t)(audio_pkt->dts / config->timescale);
                    audio_pkt->duration = (int64_t)(audio_pkt->duration / config->timescale);
                }

                ret = av_interleaved_write_frame(g_output_ctx->fmt_ctx, audio_pkt);
                if (ret < 0 && config->debug) {
                    fprintf(stderr, "Error writing audio packet: %d\n", ret);
                }

                av_packet_free(&audio_pkt);
            }
        }

        av_packet_unref(g_input_ctx->packet);
    }

    avcodec_send_packet(g_input_ctx->codec_ctx, NULL);
    while (avcodec_receive_frame(g_input_ctx->codec_ctx, decoded_frame) >= 0) {
        if (g_filter_graph) {
            av_buffersrc_add_frame_flags(g_buffersrc_ctx, decoded_frame, AV_BUFFERSRC_FLAG_KEEP_REF);
            while (av_buffersink_get_frame(g_buffersink_ctx, filtered_frame) >= 0) {
                frame_queue_push(g_frame_queue, filtered_frame);
                av_frame_unref(filtered_frame);
            }
        }
        else {
            frame_queue_push(g_frame_queue, decoded_frame);
        }
        av_frame_unref(decoded_frame);
    }

    frame_queue_signal_finished(g_frame_queue);

    if (config->verbose) {
        printf("Waiting for processing to complete...\n");
    }

#ifdef _WIN32
    WaitForSingleObject(processing_tid, INFINITE);
    CloseHandle(processing_tid);
#else
    pthread_join(processing_tid, NULL);
#endif

    av_write_trailer(g_output_ctx->fmt_ctx);

    av_frame_free(&decoded_frame);
    av_frame_free(&filtered_frame);

    if (config->verbose) {
        printf("Video processing completed\n");
    }

    return !is_interrupted();
}

bool video_get_info(const char* filename, int* width, int* height, double* fps, int64_t* frame_count) {
    AVFormatContext* fmt_ctx = NULL;
    int ret;

    ret = avformat_open_input(&fmt_ctx, filename, NULL, NULL);
    if (ret < 0) {
        return false;
    }

    ret = avformat_find_stream_info(fmt_ctx, NULL);
    if (ret < 0) {
        avformat_close_input(&fmt_ctx);
        return false;
    }

    int video_stream_idx = -1;
    for (unsigned int i = 0; i < fmt_ctx->nb_streams; i++) {
        if (fmt_ctx->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_VIDEO) {
            video_stream_idx = i;
            break;
        }
    }

    if (video_stream_idx == -1) {
        avformat_close_input(&fmt_ctx);
        return false;
    }

    AVStream* video_stream = fmt_ctx->streams[video_stream_idx];
    AVCodecParameters* codecpar = video_stream->codecpar;

    *width = codecpar->width;
    *height = codecpar->height;

    AVRational frame_rate = video_stream->avg_frame_rate;
    if (frame_rate.num == 0 || frame_rate.den == 0) {
        frame_rate = av_guess_frame_rate(fmt_ctx, video_stream, NULL);
    }
    *fps = av_q2d(frame_rate);

    *frame_count = video_stream->nb_frames;
    if (*frame_count <= 0 && fmt_ctx->duration != AV_NOPTS_VALUE) {
        *frame_count = (int64_t)(fmt_ctx->duration * (*fps) / AV_TIME_BASE);
    }

    avformat_close_input(&fmt_ctx);

    g_input_ctx = (VideoContext*)calloc(1, sizeof(VideoContext));
    if (!g_input_ctx) {
        return false;
    }

    BlurConfig dummy_config = { 0 };
    dummy_config.gpu_decoding = false;
    dummy_config.threads = 1;
    strcpy(dummy_config.gpu_type, "nvidia");

    bool result = open_input_video(g_input_ctx, filename, &dummy_config);
    if (!result && g_input_ctx) {
        free(g_input_ctx);
        g_input_ctx = NULL;
    }

    return result;
}

void video_cleanup(void) {
    if (g_frame_queue) {
        frame_queue_destroy(g_frame_queue);
        free(g_frame_queue);
        g_frame_queue = NULL;
    }

    if (g_filter_graph) {
        avfilter_graph_free(&g_filter_graph);
        g_filter_graph = NULL;
        g_buffersrc_ctx = NULL;
        g_buffersink_ctx = NULL;
    }

    if (g_input_ctx) {
        if (g_input_ctx->frame) av_frame_free(&g_input_ctx->frame);
        if (g_input_ctx->packet) av_packet_free(&g_input_ctx->packet);
        if (g_input_ctx->codec_ctx) avcodec_free_context(&g_input_ctx->codec_ctx);
        if (g_input_ctx->fmt_ctx) avformat_close_input(&g_input_ctx->fmt_ctx);
        if (g_input_ctx->hw_device_ctx) av_buffer_unref(&g_input_ctx->hw_device_ctx);
        if (g_input_ctx->sws_ctx) sws_freeContext(g_input_ctx->sws_ctx);
        free(g_input_ctx);
        g_input_ctx = NULL;
    }

    if (g_output_ctx) {
        if (g_output_ctx->frame) av_frame_free(&g_output_ctx->frame);
        if (g_output_ctx->packet) av_packet_free(&g_output_ctx->packet);
        if (g_output_ctx->codec_ctx) avcodec_free_context(&g_output_ctx->codec_ctx);
        if (g_output_ctx->fmt_ctx) {
            if (g_output_ctx->fmt_ctx->pb) avio_closep(&g_output_ctx->fmt_ctx->pb);
            avformat_free_context(g_output_ctx->fmt_ctx);
        }
        free(g_output_ctx);
        g_output_ctx = NULL;
    }

#ifdef HAVE_VAPOURSYNTH
    if (g_vs_ctx) {
        if (g_vs_ctx->node) g_vs_ctx->vsapi->freeNode(g_vs_ctx->node);
        if (g_vs_ctx->script) vsscript_freeScript(g_vs_ctx->script);
        if (g_vs_ctx->core) g_vs_ctx->vsapi->freeCore(g_vs_ctx->core);
        if (g_vs_ctx->library) {
#ifdef _WIN32
            FreeLibrary(g_vs_ctx->library);
#else
            dlclose(g_vs_ctx->library);
#endif
        }
        free(g_vs_ctx);
        g_vs_ctx = NULL;
    }
#endif
}