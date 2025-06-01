#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>
#include <math.h>
#include <time.h>
#include <signal.h>

#ifdef _WIN32
#include <windows.h>
#include <io.h>
#include "getopt.h"
#define strcasecmp _stricmp
#define strncasecmp _strnicmp
typedef HANDLE pthread_mutex_t;
typedef HANDLE pthread_t;
#define PTHREAD_MUTEX_INITIALIZER NULL
#else
#include <unistd.h>
#include <sys/time.h>
#include <getopt.h>
#include <pthread.h>
#endif

#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/opt.h>
#include <libavutil/imgutils.h>
#include <libswscale/swscale.h>
#include <libavfilter/avfilter.h>
#include <libavfilter/buffersink.h>
#include <libavfilter/buffersrc.h>

#ifdef HAVE_VAPOURSYNTH
#include <vapoursynth/VapourSynth.h>
#include <vapoursynth/VSHelper.h>
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

extern BlurConfig* config_create(void);
extern void config_destroy(BlurConfig* config);
extern bool config_load_file(BlurConfig* config, const char* filename);
extern bool config_parse_args(BlurConfig* config, int argc, char* argv[]);
extern void config_print(const BlurConfig* config);
extern bool config_validate(const BlurConfig* config);
extern float* config_get_weights(const BlurConfig* config, int frame_count, int* weight_count);

extern bool video_process(const BlurConfig* config);
extern bool video_get_info(const char* filename, int* width, int* height, double* fps, int64_t* frame_count);
extern void video_cleanup(void);

static volatile bool g_interrupted = false;

#ifdef _WIN32
static HANDLE g_progress_mutex = NULL;
#else
static pthread_mutex_t g_progress_mutex = PTHREAD_MUTEX_INITIALIZER;
#endif

static int64_t g_total_frames = 0;
static int64_t g_processed_frames = 0;
static time_t g_start_time = 0;

#ifdef _WIN32
static void mutex_lock(HANDLE* mutex) {
    if (*mutex == NULL) {
        *mutex = CreateMutex(NULL, FALSE, NULL);
    }
    WaitForSingleObject(*mutex, INFINITE);
}

static void mutex_unlock(HANDLE mutex) {
    if (mutex) {
        ReleaseMutex(mutex);
    }
}

static void mutex_destroy(HANDLE mutex) {
    if (mutex) {
        CloseHandle(mutex);
    }
}
#else
static void mutex_lock(pthread_mutex_t* mutex) {
    pthread_mutex_lock(mutex);
}

static void mutex_unlock(pthread_mutex_t* mutex) {
    pthread_mutex_unlock(mutex);
}

static void mutex_destroy(pthread_mutex_t* mutex) {
    pthread_mutex_destroy(mutex);
}
#endif

static void signal_handler(int sig) {
    g_interrupted = true;
    fprintf(stderr, "\nInterrupted by signal %d\n", sig);
}

static void print_progress(int64_t current, int64_t total) {
    if (!total) return;

    mutex_lock(&g_progress_mutex);

    float percent = (float)current / total * 100.0f;
    time_t current_time = time(NULL);
    double elapsed = difftime(current_time, g_start_time);

    if (elapsed > 0) {
        double fps = current / elapsed;
        double eta = (total - current) / fps;

        fprintf(stderr, "\rProgress: %5.1f%% [%lld/%lld] FPS: %.1f ETA: %02d:%02d:%02d",
            percent, (long long)current, (long long)total, fps,
            (int)(eta / 3600), (int)(eta / 60) % 60, (int)eta % 60);
        fflush(stderr);
    }

    mutex_unlock(&g_progress_mutex);
}

static void print_usage(const char* program) {
    printf("Motion Blur Video Processing Application\n");
    printf("Usage: %s [options] input_file\n\n", program);

    printf("Options:\n");
    printf("  -h, --help                    Show this help message\n");
    printf("  -o, --output FILE             Output file path (required)\n");
    printf("  -c, --config FILE             Load configuration from JSON file\n");
    printf("  --blur-amount FLOAT           Motion blur intensity (0-1+, default: 1.0)\n");
    printf("  --blur-output-fps FPS         Output framerate (number or multiplier like 5x)\n");
    printf("  --blur-weighting METHOD       Weighting function (gaussian_sym, equal, vegas, etc.)\n");
    printf("  --interpolate                 Enable frame interpolation\n");
    printf("  --interpolated-fps FPS        Target interpolation framerate\n");
    printf("  --interpolation-method METHOD Interpolation algorithm (rife, svp)\n");
    printf("  --gpu                         Enable GPU acceleration\n");
    printf("  --gpu-type TYPE               GPU vendor (nvidia, amd, intel)\n");
    printf("  --quality CRF                 Video quality (0-51, default: 20)\n");
    printf("  --deduplicate                 Remove duplicate frames\n");
    printf("  --preset NAME                 Use predefined configuration preset\n");
    printf("  --verbose                     Enable verbose logging\n");
    printf("  --debug                       Enable debug mode\n");
    printf("  --threads N                   Number of processing threads\n");
    printf("  --container FORMAT            Output container (mp4, mkv, avi)\n");
    printf("  --codec CODEC                 Video codec (h264, h265, av1)\n");
    printf("  --bitrate KBPS                Target bitrate in kilobits/sec\n");
    printf("  --brightness FLOAT            Brightness adjustment (-1 to 1)\n");
    printf("  --saturation FLOAT            Saturation adjustment (-1 to 1)\n");
    printf("  --contrast FLOAT              Contrast adjustment (-1 to 1)\n");
    printf("  --gamma FLOAT                 Gamma correction (0.1 to 10)\n");
    printf("  --timescale FLOAT             Video speed multiplier\n");
    printf("  --pitch-correction            Maintain audio pitch when changing speed\n");
    printf("  --ffmpeg-filters FILTERS      Custom FFmpeg filter chain\n");
    printf("\n");

    printf("Weighting Functions:\n");
    printf("  equal          - Uniform frame blending\n");
    printf("  gaussian_sym   - Symmetric Gaussian distribution (default)\n");
    printf("  gaussian       - Standard Gaussian curve\n");
    printf("  vegas          - Vegas-style weighting\n");
    printf("  pyramid        - Pyramidal distribution\n");
    printf("  ascending      - Increasing weights\n");
    printf("  descending     - Decreasing weights\n");
    printf("  gaussian_reverse - Inverted Gaussian\n");
    printf("\n");

    printf("Presets:\n");
    printf("  gaming         - Low blur for gameplay footage\n");
    printf("  cinematic      - Balanced blur for film content\n");
    printf("  smooth         - High blur for maximum smoothness\n");
    printf("\n");

    printf("Examples:\n");
    printf("  %s -o output.mp4 --blur-amount 1.0 input.mp4\n", program);
    printf("  %s -o smooth.mp4 --interpolate --interpolated-fps 5x --gpu input.mp4\n", program);
    printf("  %s -c config.json -o result.mp4 gameplay.mp4\n", program);
    printf("  %s --preset gaming -o gameplay_blur.mp4 --gpu recording.mp4\n", program);
}

static bool validate_output_path(const char* path) {
    if (!path || strlen(path) == 0) return false;

    char* dir_sep = strrchr(path, '/');
    if (!dir_sep) dir_sep = strrchr(path, '\\');

    if (dir_sep) {
        char dir[512];
        size_t dir_len = dir_sep - path;
        if (dir_len >= sizeof(dir)) return false;

        strncpy(dir, path, dir_len);
        dir[dir_len] = '\0';

#ifdef _WIN32
        if (_access(dir, 0) != 0) {
            fprintf(stderr, "Error: Output directory does not exist: %s\n", dir);
            return false;
        }
#else
        if (access(dir, F_OK) != 0) {
            fprintf(stderr, "Error: Output directory does not exist: %s\n", dir);
            return false;
        }
#endif
    }

    FILE* test = fopen(path, "ab");
    if (!test) {
        fprintf(stderr, "Error: Cannot write to output path: %s\n", path);
        return false;
    }
    fclose(test);
    remove(path);

    return true;
}

static void print_summary(const BlurConfig* config) {
    printf("\nProcessing Configuration:\n");
    printf("------------------------\n");
    printf("Input:  %s\n", config->input_file);
    printf("Output: %s\n", config->output_file);

    if (config->blur) {
        printf("Blur:   %.2f amount, %s weighting\n",
            config->blur_amount, config->blur_weighting);
    }

    if (config->interpolate) {
        printf("Interpolation: %s method, %s fps\n",
            config->interpolation_method, config->interpolated_fps);
    }

    if (config->gpu_encoding || config->gpu_decoding || config->gpu_interpolation) {
        printf("GPU:    %s acceleration enabled\n", config->gpu_type);
    }

    printf("Quality: CRF %d\n", config->quality);
    printf("Threads: %d\n", config->threads);
    printf("\n");
}

static void print_version(void) {
    printf("SwuabBlur Motion Blur Video Processor v1.0\n");
    printf("Built with FFmpeg %s\n", av_version_info());
    printf("Copyright (c) 2024 SwuabBlur Contributors\n");
    printf("\nSupported features:\n");
    printf("  - Motion blur with multiple weighting functions\n");
    printf("  - Frame interpolation (RIFE, SVP)\n");
    printf("  - GPU acceleration (NVIDIA, AMD, Intel)\n");
    printf("  - Duplicate frame detection\n");
    printf("  - Custom FFmpeg filter chains\n");
    printf("  - Audio processing with pitch correction\n");
#ifdef HAVE_VAPOURSYNTH
    printf("  - VapourSynth integration\n");
#endif
    printf("\n");
}

int main(int argc, char* argv[]) {
    signal(SIGINT, signal_handler);
    signal(SIGTERM, signal_handler);

#ifdef _WIN32
    SetConsoleOutputCP(CP_UTF8);
    g_progress_mutex = CreateMutex(NULL, FALSE, NULL);
#endif

    BlurConfig* config = config_create();
    if (!config) {
        fprintf(stderr, "Error: Failed to create configuration\n");
        return 1;
    }

    if (argc < 2) {
        print_usage(argv[0]);
        config_destroy(config);
        return 1;
    }

    if (strcmp(argv[1], "-h") == 0 || strcmp(argv[1], "--help") == 0) {
        print_usage(argv[0]);
        config_destroy(config);
        return 0;
    }

    if (strcmp(argv[1], "--version") == 0) {
        print_version();
        config_destroy(config);
        return 0;
    }

    bool has_config_file = false;
    for (int i = 1; i < argc - 1; i++) {
        if (strcmp(argv[i], "-c") == 0 || strcmp(argv[i], "--config") == 0) {
            if (i + 1 < argc) {
                if (!config_load_file(config, argv[i + 1])) {
                    fprintf(stderr, "Error: Failed to load config file: %s\n", argv[i + 1]);
                    config_destroy(config);
                    return 1;
                }
                has_config_file = true;
                break;
            }
        }
    }

    if (!config_parse_args(config, argc, argv)) {
        if (!has_config_file) {
            print_usage(argv[0]);
        }
        config_destroy(config);
        return 1;
    }

    if (strlen(config->output_file) == 0) {
        fprintf(stderr, "Error: Output file not specified\n");
        print_usage(argv[0]);
        config_destroy(config);
        return 1;
    }

    if (!validate_output_path(config->output_file)) {
        config_destroy(config);
        return 1;
    }

    if (!config_validate(config)) {
        config_destroy(config);
        return 1;
    }

    if (config->verbose) {
        config_print(config);
    }

    printf("Analyzing input video...\n");
    int width, height;
    double fps;
    int64_t frame_count;

    if (!video_get_info(config->input_file, &width, &height, &fps, &frame_count)) {
        fprintf(stderr, "Error: Failed to analyze input video\n");
        config_destroy(config);
        return 1;
    }

    printf("Input video: %dx%d @ %.2f fps, %lld frames\n",
        width, height, fps, (long long)frame_count);

    print_summary(config);

    g_total_frames = frame_count;
    g_processed_frames = 0;
    g_start_time = time(NULL);

    printf("Starting video processing...\n");
    bool success = video_process(config);

    if (g_interrupted) {
        fprintf(stderr, "\nProcessing interrupted by user\n");
        remove(config->output_file);
    }
    else if (success) {
        fprintf(stderr, "\nProcessing completed successfully\n");

        time_t end_time = time(NULL);
        double total_time = difftime(end_time, g_start_time);
        printf("Total processing time: %02d:%02d:%02d\n",
            (int)(total_time / 3600),
            (int)(total_time / 60) % 60,
            (int)total_time % 60);

        if (g_processed_frames > 0 && total_time > 0) {
            printf("Average processing speed: %.2f fps\n",
                (double)g_processed_frames / total_time);
        }
    }
    else {
        fprintf(stderr, "\nProcessing failed\n");
        remove(config->output_file);
    }

    video_cleanup();
    config_destroy(config);

#ifdef _WIN32
    mutex_destroy(g_progress_mutex);
#endif

    return success ? 0 : 1;
}

void update_progress(int64_t frames) {
    g_processed_frames = frames;
    print_progress(g_processed_frames, g_total_frames);
}

bool is_interrupted(void) {
    return g_interrupted;
}