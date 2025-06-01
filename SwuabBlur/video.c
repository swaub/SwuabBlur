#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>
#include <math.h>
#include <pthread.h>

#ifdef _WIN32
#include <windows.h>
#else
#include <dlfcn.h>
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

#include <vapoursynth/VapourSynth.h>
#include <vapoursynth/VSHelper.h>

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

typedef struct {
    VSCore* core;
    VSScript* script;
    VSNodeRef* node;
    const VSAPI* vsapi;
    void* library;
} VapourSynthContext;

typedef struct {
    uint8_t** data;
    int* linesize;
    int width;
    int height;
    int64_t pts;
} FrameBuffer;

typedef struct {
    FrameBuffer* frames;
    int capacity;
    int count;
    int read_pos;
    int write_pos;
    pthread_mutex_t mutex;
    pthread_cond_t not_empty;
    pthread_cond_t not_full;
} FrameQueue;

static VideoContext* g_input_ctx = NULL;
static VideoContext* g_output_ctx = NULL;
static VapourSynthContext* g_vs_ctx = NULL;
static FrameQueue* g_frame_queue = NULL;
static AVFilterGraph* g_filter_graph = NULL;
static AVFilterContext* g_buffersrc_ctx = NULL;
static AVFilterContext* g_buffersink_ctx = NULL;

extern void update_progress(int64_t frames);
extern bool is_interrupted(void);
extern float* config_get_weights(const BlurConfig* config, int frame_count, int* weight_count);

static void frame_queue_init(FrameQueue* queue, int capacity) {
    queue->frames = (FrameBuffer*)calloc(capacity, sizeof(FrameBuffer));
    queue->capacity = capacity;
    queue->count = 0;
    queue->read_pos = 0;
    queue->write_pos = 0;
    pthread_mutex_init(&queue->mutex, NULL);
    pthread_cond_init(&queue->not_empty, NULL);
    pthread_cond_init(&queue->not_full, NULL);
}

static void frame_queue_destroy(FrameQueue* queue) {
    if (!queue) return;

    for (int i = 0; i < queue->capacity; i++) {
        if (queue->frames[i].data) {
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

static bool frame_queue_push(FrameQueue* queue, AVFrame* frame) {
    pthread_mutex_lock(&queue->mutex);

    while (queue->count >= queue->capacity && !is_interrupted()) {
        pthread_cond_wait(&queue->not_full, &queue->mutex);
    }

    if (is_interrupted()) {
        pthread_mutex_unlock(&queue->mutex);
        return false;
    }

    FrameBuffer* buffer = &queue->frames[queue->write_pos];

    if (!buffer->data) {
        buffer->data = (uint8_t**)av_malloc(4 * sizeof(uint8_t*));
        buffer->linesize = (int*)av_malloc(4 * sizeof(int));
        buffer->width = frame->width;
        buffer->height = frame->height;

        for (int i = 0; i < 4; i++) {
            if (frame->data[i]) {
                int size = frame->linesize[i] * frame->height;
                if (i > 0) size >>= 1;
                buffer->data[i] = (uint8_t*)av_malloc(size);
                buffer->linesize[i] = frame->linesize[i];
            }
            else {
                buffer->data[i] = NULL;
                buffer->linesize[i] = 0;
            }
        }
    }

    for (int i = 0; i < 4; i++) {
        if (frame->data[i]) {
            int h = frame->height;
            if (i > 0) h >>= 1;
            for (int y = 0; y < h; y++) {
                memcpy(buffer->data[i] + y * buffer->linesize[i],
                    frame->data[i] + y * frame->linesize[i],
                    frame->linesize[i]);
            }
        }
    }

    buffer->pts = frame->pts;

    queue->write_pos = (queue->write_pos + 1) % queue->capacity;
    queue->count++;

    pthread_cond_signal(&queue->not_empty);
    pthread_mutex_unlock(&queue->mutex);

    return true;
}

static bool frame_queue_pop(FrameQueue* queue, AVFrame* frame) {
    pthread_mutex_lock(&queue->mutex);

    while (queue->count == 0 && !is_interrupted()) {
        pthread_cond_wait(&queue->not_empty, &queue->mutex);
    }

    if (is_interrupted()) {
        pthread_mutex_unlock(&queue->mutex);
        return false;
    }

    FrameBuffer* buffer = &queue->frames[queue->read_pos];

    for (int i = 0; i < 4; i++) {
        frame->data[i] = buffer->data[i];
        frame->linesize[i] = buffer->linesize[i];
    }

    frame->width = buffer->width;
    frame->height = buffer->height;
    frame->pts = buffer->pts;
    frame->format = AV_PIX_FMT_YUV420P;

    queue->read_pos = (queue->read_pos + 1) % queue->capacity;
    queue->count--;

    pthread_cond_signal(&queue->not_full);
    pthread_mutex_unlock(&queue->mutex);

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
    else if (strcmp(codec, "h265") == 0) {
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
        return AV_HWDEVICE_TYPE_D3D11VA;
    }
    else if (strcmp(gpu_type, "intel") == 0) {
        return AV_HWDEVICE_TYPE_QSV;
    }
    return AV_HWDEVICE_TYPE_NONE;
}

static bool open_input_video(VideoContext* ctx, const char* filename, const BlurConfig* config) {
    int ret;

    ctx->fmt_ctx = NULL;
    ret = avformat_open_input(&ctx->fmt_ctx, filename, NULL, NULL);
    if (ret < 0) {
        fprintf(stderr, "Error opening input file: %s\n", filename);
        return false;
    }

    ret = avformat_find_stream_info(ctx->fmt_ctx, NULL);
    if (ret < 0) {
        fprintf(stderr, "Error finding stream info\n");
        return false;
    }

    ctx->video_stream_idx = -1;
    ctx->audio_stream_idx = -1;

    for (unsigned int i = 0; i < ctx->fmt_ctx->nb_streams; i++) {
        if (ctx->fmt_ctx->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_VIDEO) {
            ctx->video_stream_idx = i;
            ctx->video_stream = ctx->fmt_ctx->streams[i];
        }
        else if (ctx->fmt_ctx->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_AUDIO) {
            ctx->audio_stream_idx = i;
            ctx->audio_stream = ctx->fmt_ctx->streams[i];
        }
    }

    if (ctx->video_stream_idx == -1) {
        fprintf(stderr, "No video stream found\n");
        return false;
    }

    const char* codec_name = NULL;
    if (config->gpu_decoding) {
        enum AVHWDeviceType hw_type = get_hw_device_type(config->gpu_type);
        if (hw_type != AV_HWDEVICE_TYPE_NONE) {
            ret = av_hwdevice_ctx_create(&ctx->hw_device_ctx, hw_type, NULL, NULL, 0);
            if (ret < 0) {
                fprintf(stderr, "Failed to create hardware device context\n");
                ctx->hw_device_ctx = NULL;
            }
            else {
                codec_name = get_hw_codec_name(
                    avcodec_get_name(ctx->video_stream->codecpar->codec_id),
                    config->gpu_type, false);
            }
        }
    }

    const AVCodec* codec = codec_name ?
        avcodec_find_decoder_by_name(codec_name) :
        avcodec_find_decoder(ctx->video_stream->codecpar->codec_id);

    if (!codec) {
        fprintf(stderr, "Codec not found\n");
        return false;
    }

    ctx->codec_ctx = avcodec_alloc_context3(codec);
    if (!ctx->codec_ctx) {
        fprintf(stderr, "Failed to allocate codec context\n");
        return false;
    }

    ret = avcodec_parameters_to_context(ctx->codec_ctx, ctx->video_stream->codecpar);
    if (ret < 0) {
        fprintf(stderr, "Failed to copy codec parameters\n");
        return false;
    }

    if (ctx->hw_device_ctx) {
        ctx->codec_ctx->hw_device_ctx = av_buffer_ref(ctx->hw_device_ctx);
    }

    if (config->threads > 0) {
        ctx->codec_ctx->thread_count = config->threads;
    }

    ret = avcodec_open2(ctx->codec_ctx, codec, NULL);
    if (ret < 0) {
        fprintf(stderr, "Failed to open codec\n");
        return false;
    }

    ctx->frame = av_frame_alloc();
    ctx->packet = av_packet_alloc();

    if (!ctx->frame || !ctx->packet) {
        fprintf(stderr, "Failed to allocate frame/packet\n");
        return false;
    }

    return true;
}

static bool create_output_video(VideoContext* ctx, const char* filename, const BlurConfig* config,
    int width, int height, double fps) {
    int ret;

    avformat_alloc_output_context2(&ctx->fmt_ctx, NULL, NULL, filename);
    if (!ctx->fmt_ctx) {
        fprintf(stderr, "Failed to create output context\n");
        return false;
    }

    const char* codec_name = NULL;
    if (config->gpu_encoding) {
        codec_name = get_hw_codec_name(config->codec, config->gpu_type, true);
    }

    const AVCodec* codec = codec_name ?
        avcodec_find_encoder_by_name(codec_name) :
        avcodec_find_encoder_by_name(config->codec);

    if (!codec) {
        codec = avcodec_find_encoder(AV_CODEC_ID_H264);
    }

    ctx->video_stream = avformat_new_stream(ctx->fmt_ctx, codec);
    if (!ctx->video_stream) {
        fprintf(stderr, "Failed to create video stream\n");
        return false;
    }

    ctx->codec_ctx = avcodec_alloc_context3(codec);
    if (!ctx->codec_ctx) {
        fprintf(stderr, "Failed to allocate codec context\n");
        return false;
    }

    ctx->codec_ctx->width = width;
    ctx->codec_ctx->height = height;
    ctx->codec_ctx->time_base = (AVRational){ 1, (int)fps };
    ctx->codec_ctx->framerate = (AVRational){ (int)fps, 1 };
    ctx->codec_ctx->gop_size = (int)fps;
    ctx->codec_ctx->max_b_frames = 2;
    ctx->codec_ctx->pix_fmt = AV_PIX_FMT_YUV420P;

    if (config->gpu_encoding && ctx->hw_device_ctx) {
        ctx->codec_ctx->hw_device_ctx = av_buffer_ref(ctx->hw_device_ctx);
    }

    if (config->bitrate > 0) {
        ctx->codec_ctx->bit_rate = config->bitrate * 1000;
    }
    else {
        av_opt_set_int(ctx->codec_ctx->priv_data, "crf", config->quality, 0);
    }

    if (config->threads > 0) {
        ctx->codec_ctx->thread_count = config->threads;
    }

    if (ctx->fmt_ctx->oformat->flags & AVFMT_GLOBALHEADER) {
        ctx->codec_ctx->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;
    }

    ret = avcodec_open2(ctx->codec_ctx, codec, NULL);
    if (ret < 0) {
        fprintf(stderr, "Failed to open encoder\n");
        return false;
    }

    ret = avcodec_parameters_from_context(ctx->video_stream->codecpar, ctx->codec_ctx);
    if (ret < 0) {
        fprintf(stderr, "Failed to copy codec parameters\n");
        return false;
    }

    ctx->video_stream->time_base = ctx->codec_ctx->time_base;

    if (g_input_ctx->audio_stream_idx >= 0) {
        AVStream* in_stream = g_input_ctx->fmt_ctx->streams[g_input_ctx->audio_stream_idx];
        AVStream* out_stream = avformat_new_stream(ctx->fmt_ctx, NULL);

        if (out_stream) {
            ret = avcodec_parameters_copy(out_stream->codecpar, in_stream->codecpar);
            if (ret >= 0) {
                out_stream->time_base = in_stream->time_base;
                ctx->audio_stream = out_stream;
                ctx->audio_stream_idx = out_stream->index;
            }
        }
    }

    ret = avio_open(&ctx->fmt_ctx->pb, filename, AVIO_FLAG_WRITE);
    if (ret < 0) {
        fprintf(stderr, "Failed to open output file\n");
        return false;
    }

    ret = avformat_write_header(ctx->fmt_ctx, NULL);
    if (ret < 0) {
        fprintf(stderr, "Failed to write header\n");
        return false;
    }

    ctx->frame = av_frame_alloc();
    ctx->packet = av_packet_alloc();

    return true;
}

static bool init_vapoursynth(VapourSynthContext* vs_ctx) {
#ifdef _WIN32
    vs_ctx->library = LoadLibrary("vapoursynth.dll");
#else
    vs_ctx->library = dlopen("libvapoursynth.so", RTLD_LAZY);
#endif

    if (!vs_ctx->library) {
        fprintf(stderr, "Failed to load VapourSynth library\n");
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
        fprintf(stderr, "Failed to get VapourSynth API function\n");
        return false;
    }

    vs_ctx->vsapi = getVapourSynthAPI(VAPOURSYNTH_API_VERSION);
    if (!vs_ctx->vsapi) {
        fprintf(stderr, "Failed to get VapourSynth API\n");
        return false;
    }

    vs_ctx->core = vs_ctx->vsapi->createCore(0);
    if (!vs_ctx->core) {
        fprintf(stderr, "Failed to create VapourSynth core\n");
        return false;
    }

    return true;
}

static char* generate_vapoursynth_script(const BlurConfig* config, int width, int height, double fps) {
    static char script[8192];
    char interpolated_fps_str[32];
    double target_fps = fps;

    if (config->interpolate) {
        if (strchr(config->interpolated_fps, 'x')) {
            double multiplier = atof(config->interpolated_fps);
            target_fps = fps * multiplier;
        }
        else {
            target_fps = atof(config->interpolated_fps);
        }
    }

    snprintf(interpolated_fps_str, sizeof(interpolated_fps_str), "%.2f", target_fps);

    snprintf(script, sizeof(script),
        "import vapoursynth as vs\n"
        "core = vs.get_core()\n"
        "clip = core.std.BlankClip(width=%d, height=%d, format=vs.YUV420P8, length=100000, fpsnum=%d, fpsden=1)\n",
        width, height, (int)fps);

    if (config->deduplicate) {
        strcat(script, "clip = core.dedupe.DeDupe(clip, threshold=");
        char threshold_str[32];
        snprintf(threshold_str, sizeof(threshold_str), "%.2f", config->deduplicate_threshold);
        strcat(script, threshold_str);
        strcat(script, ", range=");
        char range_str[32];
        snprintf(range_str, sizeof(range_str), "%d", config->deduplicate_range);
        strcat(script, range_str);
        strcat(script, ")\n");
    }

    if (config->interpolate) {
        if (strcmp(config->interpolation_method, "svp") == 0) {
            strcat(script, "import sys\n");
            strcat(script, "sys.path.append(r'C:\\Program Files (x86)\\SVP 4\\plugins64')\n");
            strcat(script, "import vapoursynth as vs\n");
            strcat(script, "core = vs.get_core(threads=");
            char threads_str[32];
            snprintf(threads_str, sizeof(threads_str), "%d", config->threads > 0 ? config->threads : 1);
            strcat(script, threads_str);
            strcat(script, ")\n");

            strcat(script, "clip = core.svp1.Super(clip, ");
            if (config->manual_svp && strlen(config->svp_super_string) > 0) {
                strcat(script, config->svp_super_string);
            }
            else {
                strcat(script, "{pel:2,gpu:1}");
            }
            strcat(script, ")\n");

            strcat(script, "vectors = core.svp1.Analyse(clip, ");
            if (config->manual_svp && strlen(config->svp_vectors_string) > 0) {
                strcat(script, config->svp_vectors_string);
            }
            else {
                strcat(script, "{block:{w:");
                char block_str[32];
                snprintf(block_str, sizeof(block_str), "%d", config->interpolation_block_size);
                strcat(script, block_str);
                strcat(script, ",h:");
                strcat(script, block_str);
                strcat(script, "}}");
            }
            strcat(script, ")\n");

            strcat(script, "clip = core.svp2.SmoothFps(clip, clip, vectors, ");
            if (config->manual_svp && strlen(config->svp_smooth_string) > 0) {
                strcat(script, config->svp_smooth_string);
            }
            else {
                strcat(script, "{rate:{num:");
                strcat(script, interpolated_fps_str);
                strcat(script, ",den:1},algo:");
                char algo_str[32];
                snprintf(algo_str, sizeof(algo_str), "%d", config->svp_algorithm);
                strcat(script, algo_str);
                strcat(script, ",mask:{area:");
                char mask_str[32];
                snprintf(mask_str, sizeof(mask_str), "%.0f", config->interpolation_mask_area * 100);
                strcat(script, mask_str);
                strcat(script, "}}");
            }
            strcat(script, ")\n");
        }
        else if (strcmp(config->interpolation_method, "rife") == 0) {
            strcat(script, "clip = core.rife.RIFE(clip, factor=");
            char factor_str[32];
            snprintf(factor_str, sizeof(factor_str), "%.1f", target_fps / fps);
            strcat(script, factor_str);
            if (config->gpu_interpolation) {
                strcat(script, ", gpu_id=0");
            }
            strcat(script, ")\n");
        }
    }

    if (config->brightness != 0 || config->saturation != 0 ||
        config->contrast != 0 || config->gamma != 1.0) {
        strcat(script, "clip = core.std.Levels(clip, ");

        int min_in = 0, max_in = 255;
        int min_out = 0, max_out = 255;

        if (config->brightness != 0) {
            int brightness_offset = (int)(config->brightness * 255);
            min_out = CLAMP(min_out + brightness_offset, 0, 255);
            max_out = CLAMP(max_out + brightness_offset, 0, 255);
        }

        if (config->contrast != 0) {
            float contrast_factor = 1.0f + config->contrast;
            int mid = 128;
            min_in = CLAMP((int)(mid - (mid - min_in) * contrast_factor), 0, 255);
            max_in = CLAMP((int)(mid + (max_in - mid) * contrast_factor), 0, 255);
        }

        char levels_str[256];
        snprintf(levels_str, sizeof(levels_str),
            "min_in=%d, max_in=%d, min_out=%d, max_out=%d, gamma=%.2f",
            min_in, max_in, min_out, max_out, config->gamma);
        strcat(script, levels_str);
        strcat(script, ")\n");
    }

    if (config->timescale != 1.0) {
        strcat(script, "clip = core.std.AssumeFPS(clip, fpsnum=");
        char scaled_fps_str[32];
        snprintf(scaled_fps_str, sizeof(scaled_fps_str), "%d", (int)(fps * config->timescale));
        strcat(script, scaled_fps_str);
        strcat(script, ", fpsden=1)\n");
    }

    strcat(script, "clip.set_output()\n");

    return script;
}

static bool create_filter_graph(const BlurConfig* config, int width, int height, double fps) {
    int ret;
    char filter_str[2048];

    g_filter_graph = avfilter_graph_alloc();
    if (!g_filter_graph) {
        return false;
    }

    snprintf(filter_str, sizeof(filter_str),
        "video_size=%dx%d:pix_fmt=%d:time_base=1/%d:pixel_aspect=1/1",
        width, height, AV_PIX_FMT_YUV420P, (int)fps);

    const AVFilter* buffersrc = avfilter_get_by_name("buffer");
    ret = avfilter_graph_create_filter(&g_buffersrc_ctx, buffersrc, "in",
        filter_str, NULL, g_filter_graph);
    if (ret < 0) {
        fprintf(stderr, "Failed to create buffer source\n");
        return false;
    }

    const AVFilter* buffersink = avfilter_get_by_name("buffersink");
    ret = avfilter_graph_create_filter(&g_buffersink_ctx, buffersink, "out",
        NULL, NULL, g_filter_graph);
    if (ret < 0) {
        fprintf(stderr, "Failed to create buffer sink\n");
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

    filter_str[0] = '\0';

    if (strlen(config->ffmpeg_filters) > 0) {
        strcat(filter_str, config->ffmpeg_filters);
    }
    else {
        strcat(filter_str, "null");
    }

    ret = avfilter_graph_parse_ptr(g_filter_graph, filter_str, &inputs, &outputs, NULL);
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

static bool apply_motion_blur(AVFrame** frames, int frame_count, float* weights, AVFrame* output) {
    if (frame_count == 0) return false;

    int width = frames[0]->width;
    int height = frames[0]->height;

    for (int y = 0; y < height; y++) {
        for (int x = 0; x < width; x++) {
            float y_sum = 0, u_sum = 0, v_sum = 0;

            for (int i = 0; i < frame_count; i++) {
                if (!frames[i]) continue;

                uint8_t y_val = frames[i]->data[0][y * frames[i]->linesize[0] + x];
                uint8_t u_val = frames[i]->data[1][(y / 2) * frames[i]->linesize[1] + (x / 2)];
                uint8_t v_val = frames[i]->data[2][(y / 2) * frames[i]->linesize[2] + (x / 2)];

                y_sum += y_val * weights[i];
                u_sum += u_val * weights[i];
                v_sum += v_val * weights[i];
            }

            output->data[0][y * output->linesize[0] + x] = (uint8_t)CLAMP(y_sum, 0, 255);
            if (y % 2 == 0 && x % 2 == 0) {
                output->data[1][(y / 2) * output->linesize[1] + (x / 2)] = (uint8_t)CLAMP(u_sum, 0, 255);
                output->data[2][(y / 2) * output->linesize[2] + (x / 2)] = (uint8_t)CLAMP(v_sum, 0, 255);
            }
        }
    }

    return true;
}

static bool detect_duplicate_frames(AVFrame* frame1, AVFrame* frame2, float threshold) {
    if (!frame1 || !frame2) return false;

    int width = frame1->width;
    int height = frame1->height;
    int total_pixels = width * height;

    int64_t diff_sum = 0;

    for (int y = 0; y < height; y++) {
        for (int x = 0; x < width; x++) {
            int diff = abs(frame1->data[0][y * frame1->linesize[0] + x] -
                frame2->data[0][y * frame2->linesize[0] + x]);
            diff_sum += diff;
        }
    }

    float avg_diff = (float)diff_sum / total_pixels;
    return avg_diff < threshold * 255;
}

static void* processing_thread(void* arg) {
    BlurConfig* config = (BlurConfig*)arg;
    AVFrame** blur_frames = NULL;
    int blur_frame_count = 0;
    int weight_count = 0;
    float* weights = NULL;

    double input_fps = av_q2d(g_input_ctx->video_stream->avg_frame_rate);
    double output_fps = input_fps;

    if (strchr(config->blur_output_fps, 'x')) {
        double multiplier = atof(config->blur_output_fps);
        output_fps = input_fps * multiplier;
    }
    else {
        output_fps = atof(config->blur_output_fps);
    }

    blur_frame_count = (int)(output_fps / input_fps * config->blur_amount + 0.5);
    if (blur_frame_count < 1) blur_frame_count = 1;

    weights = config_get_weights(config, blur_frame_count, &weight_count);
    if (!weights) {
        fprintf(stderr, "Failed to generate weights\n");
        return NULL;
    }

    blur_frames = (AVFrame**)calloc(blur_frame_count, sizeof(AVFrame*));
    if (!blur_frames) {
        free(weights);
        return NULL;
    }

    for (int i = 0; i < blur_frame_count; i++) {
        blur_frames[i] = av_frame_alloc();
        if (!blur_frames[i]) {
            for (int j = 0; j < i; j++) {
                av_frame_free(&blur_frames[j]);
            }
            free(blur_frames);
            free(weights);
            return NULL;
        }
    }

    int current_frame = 0;
    AVFrame* output_frame = av_frame_alloc();

    while (!is_interrupted()) {
        AVFrame* input_frame = av_frame_alloc();

        if (!frame_queue_pop(g_frame_queue, input_frame)) {
            av_frame_free(&input_frame);
            break;
        }

        if (config->deduplicate && current_frame > 0) {
            bool is_duplicate = false;
            for (int i = 0; i < config->deduplicate_range && i < current_frame; i++) {
                if (detect_duplicate_frames(input_frame, blur_frames[i], config->deduplicate_threshold)) {
                    is_duplicate = true;
                    break;
                }
            }

            if (is_duplicate) {
                av_frame_free(&input_frame);
                continue;
            }
        }

        av_frame_unref(blur_frames[current_frame % blur_frame_count]);
        av_frame_ref(blur_frames[current_frame % blur_frame_count], input_frame);

        if (current_frame >= blur_frame_count - 1) {
            av_frame_unref(output_frame);
            output_frame->format = AV_PIX_FMT_YUV420P;
            output_frame->width = input_frame->width;
            output_frame->height = input_frame->height;
            av_frame_get_buffer(output_frame, 0);

            AVFrame** ordered_frames = (AVFrame**)calloc(blur_frame_count, sizeof(AVFrame*));
            for (int i = 0; i < blur_frame_count; i++) {
                int idx = (current_frame - blur_frame_count + 1 + i) % blur_frame_count;
                if (idx < 0) idx += blur_frame_count;
                ordered_frames[i] = blur_frames[idx];
            }

            apply_motion_blur(ordered_frames, blur_frame_count, weights, output_frame);
            free(ordered_frames);

            output_frame->pts = input_frame->pts;

            int ret = avcodec_send_frame(g_output_ctx->codec_ctx, output_frame);
            if (ret < 0) {
                fprintf(stderr, "Error sending frame to encoder\n");
            }

            while (ret >= 0) {
                ret = avcodec_receive_packet(g_output_ctx->codec_ctx, g_output_ctx->packet);
                if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF) {
                    break;
                }
                else if (ret < 0) {
                    fprintf(stderr, "Error receiving packet from encoder\n");
                    break;
                }

                g_output_ctx->packet->stream_index = g_output_ctx->video_stream->index;
                av_packet_rescale_ts(g_output_ctx->packet,
                    g_output_ctx->codec_ctx->time_base,
                    g_output_ctx->video_stream->time_base);

                ret = av_interleaved_write_frame(g_output_ctx->fmt_ctx, g_output_ctx->packet);
                if (ret < 0) {
                    fprintf(stderr, "Error writing frame\n");
                }

                av_packet_unref(g_output_ctx->packet);
            }
        }

        current_frame++;
        av_frame_free(&input_frame);

        if (current_frame % 10 == 0) {
            update_progress(current_frame);
        }
    }

    av_frame_free(&output_frame);

    for (int i = 0; i < blur_frame_count; i++) {
        av_frame_free(&blur_frames[i]);
    }
    free(blur_frames);
    free(weights);

    return NULL;
}

bool video_process(const BlurConfig* config) {
    int width = g_input_ctx->codec_ctx->width;
    int height = g_input_ctx->codec_ctx->height;
    double fps = av_q2d(g_input_ctx->video_stream->avg_frame_rate);

    double output_fps = fps;
    if (strchr(config->blur_output_fps, 'x')) {
        double multiplier = atof(config->blur_output_fps);
        output_fps = fps * multiplier;
    }
    else {
        output_fps = atof(config->blur_output_fps);
    }

    if (config->timescale != 1.0) {
        output_fps *= config->timescale;
    }

    g_output_ctx = (VideoContext*)calloc(1, sizeof(VideoContext));
    if (!g_output_ctx) {
        return false;
    }

    if (config->gpu_encoding) {
        g_output_ctx->hw_device_ctx = g_input_ctx->hw_device_ctx;
    }

    if (!create_output_video(g_output_ctx, config->output_file, config, width, height, output_fps)) {
        return false;
    }

    if (config->interpolate) {
        g_vs_ctx = (VapourSynthContext*)calloc(1, sizeof(VapourSynthContext));
        if (!init_vapoursynth(g_vs_ctx)) {
            fprintf(stderr, "Warning: VapourSynth initialization failed, continuing without interpolation\n");
            free(g_vs_ctx);
            g_vs_ctx = NULL;
        }
    }

    if (strlen(config->ffmpeg_filters) > 0 || config->brightness != 0 ||
        config->saturation != 0 || config->contrast != 0 || config->gamma != 1.0) {
        if (!create_filter_graph(config, width, height, fps)) {
            fprintf(stderr, "Warning: Filter graph creation failed\n");
        }
    }

    g_frame_queue = (FrameQueue*)calloc(1, sizeof(FrameQueue));
    frame_queue_init(g_frame_queue, 100);

    pthread_t processing_tid;
    pthread_create(&processing_tid, NULL, processing_thread, (void*)config);

    AVFrame* decoded_frame = av_frame_alloc();
    AVFrame* processed_frame = av_frame_alloc();
    int ret;

    while (!is_interrupted()) {
        ret = av_read_frame(g_input_ctx->fmt_ctx, g_input_ctx->packet);
        if (ret < 0) {
            break;
        }

        if (g_input_ctx->packet->stream_index == g_input_ctx->video_stream_idx) {
            ret = avcodec_send_packet(g_input_ctx->codec_ctx, g_input_ctx->packet);
            if (ret < 0) {
                fprintf(stderr, "Error sending packet to decoder\n");
                av_packet_unref(g_input_ctx->packet);
                continue;
            }

            while (ret >= 0) {
                ret = avcodec_receive_frame(g_input_ctx->codec_ctx, decoded_frame);
                if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF) {
                    break;
                }
                else if (ret < 0) {
                    fprintf(stderr, "Error receiving frame from decoder\n");
                    break;
                }

                if (g_filter_graph) {
                    ret = av_buffersrc_add_frame_flags(g_buffersrc_ctx, decoded_frame, 0);
                    if (ret < 0) {
                        fprintf(stderr, "Error feeding filter graph\n");
                        continue;
                    }

                    while (1) {
                        ret = av_buffersink_get_frame(g_buffersink_ctx, processed_frame);
                        if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF) {
                            break;
                        }
                        else if (ret < 0) {
                            fprintf(stderr, "Error getting frame from filter\n");
                            break;
                        }

                        frame_queue_push(g_frame_queue, processed_frame);
                        av_frame_unref(processed_frame);
                    }
                }
                else {
                    frame_queue_push(g_frame_queue, decoded_frame);
                }

                av_frame_unref(decoded_frame);
            }
        }
        else if (g_output_ctx->audio_stream_idx >= 0 &&
            g_input_ctx->packet->stream_index == g_input_ctx->audio_stream_idx) {
            g_input_ctx->packet->stream_index = g_output_ctx->audio_stream_idx;
            av_packet_rescale_ts(g_input_ctx->packet,
                g_input_ctx->audio_stream->time_base,
                g_output_ctx->audio_stream->time_base);

            if (config->timescale != 1.0) {
                g_input_ctx->packet->pts = (int64_t)(g_input_ctx->packet->pts / config->timescale);
                g_input_ctx->packet->dts = (int64_t)(g_input_ctx->packet->dts / config->timescale);
                g_input_ctx->packet->duration = (int64_t)(g_input_ctx->packet->duration / config->timescale);
            }

            ret = av_interleaved_write_frame(g_output_ctx->fmt_ctx, g_input_ctx->packet);
            if (ret < 0) {
                fprintf(stderr, "Error writing audio packet\n");
            }
        }

        av_packet_unref(g_input_ctx->packet);
    }

    avcodec_send_packet(g_input_ctx->codec_ctx, NULL);
    while (avcodec_receive_frame(g_input_ctx->codec_ctx, decoded_frame) >= 0) {
        frame_queue_push(g_frame_queue, decoded_frame);
        av_frame_unref(decoded_frame);
    }

    pthread_mutex_lock(&g_frame_queue->mutex);
    pthread_cond_broadcast(&g_frame_queue->not_empty);
    pthread_mutex_unlock(&g_frame_queue->mutex);

    pthread_join(processing_tid, NULL);

    avcodec_send_frame(g_output_ctx->codec_ctx, NULL);
    while (1) {
        ret = avcodec_receive_packet(g_output_ctx->codec_ctx, g_output_ctx->packet);
        if (ret == AVERROR_EOF) {
            break;
        }
        else if (ret < 0) {
            fprintf(stderr, "Error receiving final packets\n");
            break;
        }

        g_output_ctx->packet->stream_index = g_output_ctx->video_stream->index;
        av_packet_rescale_ts(g_output_ctx->packet,
            g_output_ctx->codec_ctx->time_base,
            g_output_ctx->video_stream->time_base);

        ret = av_interleaved_write_frame(g_output_ctx->fmt_ctx, g_output_ctx->packet);
        av_packet_unref(g_output_ctx->packet);
    }

    av_write_trailer(g_output_ctx->fmt_ctx);

    av_frame_free(&decoded_frame);
    av_frame_free(&processed_frame);

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
    *fps = av_q2d(video_stream->avg_frame_rate);
    *frame_count = video_stream->nb_frames;

    if (*frame_count <= 0) {
        *frame_count = (int64_t)(fmt_ctx->duration * (*fps) / AV_TIME_BASE);
    }

    avformat_close_input(&fmt_ctx);

    g_input_ctx = (VideoContext*)calloc(1, sizeof(VideoContext));
    if (!g_input_ctx) {
        return false;
    }

    BlurConfig dummy_config = { 0 };
    strcpy(dummy_config.gpu_type, "nvidia");

    return open_input_video(g_input_ctx, filename, &dummy_config);
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
}

#ifndef CLAMP
#define CLAMP(x, min, max) ((x) < (min) ? (min) : ((x) > (max) ? (max) : (x)))
#endif