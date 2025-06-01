#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>
#include <math.h>
#include <ctype.h>
#include <errno.h>

#ifdef _WIN32
#include <windows.h>
#include <io.h>
#define strcasecmp _stricmp
#define strncasecmp _strnicmp
#define access _access
#define strdup _strdup
#else
#include <unistd.h>
#endif

#ifdef _WIN32
#include "getopt.h"
#else
#include <getopt.h>
#endif

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

#ifndef CLAMP
#define CLAMP(x, min, max) ((x) < (min) ? (min) : ((x) > (max) ? (max) : (x)))
#endif

typedef struct cJSON cJSON;

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

typedef struct cJSON {
    struct cJSON* next;
    struct cJSON* prev;
    struct cJSON* child;
    int type;
    char* valuestring;
    int valueint;
    double valuedouble;
    char* string;
} cJSON;

#define cJSON_Invalid (0)
#define cJSON_False  (1 << 0)
#define cJSON_True   (1 << 1)
#define cJSON_NULL   (1 << 2)
#define cJSON_Number (1 << 3)
#define cJSON_String (1 << 4)
#define cJSON_Array  (1 << 5)
#define cJSON_Object (1 << 6)
#define cJSON_Raw    (1 << 7)

static void* cJSON_malloc(size_t size) {
    return malloc(size);
}

static void cJSON_free(void* ptr) {
    free(ptr);
}

static cJSON* cJSON_New_Item(void) {
    cJSON* node = (cJSON*)cJSON_malloc(sizeof(cJSON));
    if (node) memset(node, 0, sizeof(cJSON));
    return node;
}

static void cJSON_Delete(cJSON* item) {
    if (!item) return;

    if (item->child) cJSON_Delete(item->child);
    if (item->next) cJSON_Delete(item->next);
    if (item->string) cJSON_free(item->string);
    if (item->type == cJSON_String && item->valuestring) cJSON_free(item->valuestring);
    cJSON_free(item);
}

static const char* skip_whitespace(const char* str) {
    while (str && *str && isspace((unsigned char)*str)) str++;
    return str;
}

static const char* parse_value(cJSON* item, const char* value);

static const char* parse_string(cJSON* item, const char* str) {
    const char* ptr = str + 1;
    char* ptr2;
    char* out;
    int len = 0;
    unsigned uc, uc2;

    if (*str != '\"') return NULL;

    while (*ptr != '\"' && *ptr) {
        if (*ptr == '\\') ptr++;
        if (*ptr) ptr++;
        len++;
    }

    out = (char*)cJSON_malloc(len + 1);
    if (!out) return NULL;

    ptr = str + 1;
    ptr2 = out;
    while (*ptr != '\"' && *ptr) {
        if (*ptr != '\\') *ptr2++ = *ptr++;
        else {
            ptr++;
            switch (*ptr) {
            case 'b': *ptr2++ = '\b'; break;
            case 'f': *ptr2++ = '\f'; break;
            case 'n': *ptr2++ = '\n'; break;
            case 'r': *ptr2++ = '\r'; break;
            case 't': *ptr2++ = '\t'; break;
            case 'u':
                if (sscanf(ptr + 1, "%4x", &uc) == 1) {
                    ptr += 4;
                    if (uc >= 0xD800 && uc <= 0xDBFF) {
                        if (ptr[1] == '\\' && ptr[2] == 'u' && sscanf(ptr + 3, "%4x", &uc2) == 1) {
                            ptr += 6;
                            uc = 0x10000 + (((uc & 0x3FF) << 10) | (uc2 & 0x3FF));
                        }
                    }
                    if (uc < 0x80) *ptr2++ = (char)uc;
                    else if (uc < 0x800) {
                        *ptr2++ = (char)(0xC0 | (uc >> 6));
                        *ptr2++ = (char)(0x80 | (uc & 0x3F));
                    }
                    else if (uc < 0x10000) {
                        *ptr2++ = (char)(0xE0 | (uc >> 12));
                        *ptr2++ = (char)(0x80 | ((uc >> 6) & 0x3F));
                        *ptr2++ = (char)(0x80 | (uc & 0x3F));
                    }
                    else {
                        *ptr2++ = (char)(0xF0 | (uc >> 18));
                        *ptr2++ = (char)(0x80 | ((uc >> 12) & 0x3F));
                        *ptr2++ = (char)(0x80 | ((uc >> 6) & 0x3F));
                        *ptr2++ = (char)(0x80 | (uc & 0x3F));
                    }
                }
                break;
            default: *ptr2++ = *ptr; break;
            }
            ptr++;
        }
    }
    *ptr2 = 0;
    if (*ptr == '\"') ptr++;
    item->valuestring = out;
    item->type = cJSON_String;
    return ptr;
}

static const char* parse_number(cJSON* item, const char* num) {
    double n = 0, sign = 1, scale = 0;
    int subscale = 0, signsubscale = 1;

    if (*num == '-') {
        sign = -1;
        num++;
    }
    if (*num == '0') num++;
    else if (*num >= '1' && *num <= '9') {
        do {
            n = (n * 10.0) + (*num++ - '0');
        } while (*num >= '0' && *num <= '9');
    }
    else return NULL;

    if (*num == '.' && num[1] >= '0' && num[1] <= '9') {
        num++;
        do {
            n = (n * 10.0) + (*num++ - '0');
            scale--;
        } while (*num >= '0' && *num <= '9');
    }

    if (*num == 'e' || *num == 'E') {
        num++;
        if (*num == '+') num++;
        else if (*num == '-') {
            signsubscale = -1;
            num++;
        }
        while (*num >= '0' && *num <= '9') {
            subscale = (subscale * 10) + (*num++ - '0');
        }
    }

    n = sign * n * pow(10.0, scale + subscale * signsubscale);

    item->valuedouble = n;
    item->valueint = (int)n;
    item->type = cJSON_Number;
    return num;
}

static const char* parse_array(cJSON* item, const char* value) {
    cJSON* child;
    if (*value != '[') return NULL;

    item->type = cJSON_Array;
    value = skip_whitespace(value + 1);
    if (*value == ']') return value + 1;

    item->child = child = cJSON_New_Item();
    if (!child) return NULL;

    value = skip_whitespace(parse_value(child, skip_whitespace(value)));
    if (!value) return NULL;

    while (*value == ',') {
        cJSON* new_item = cJSON_New_Item();
        if (!new_item) return NULL;
        child->next = new_item;
        new_item->prev = child;
        child = new_item;
        value = skip_whitespace(parse_value(child, skip_whitespace(value + 1)));
        if (!value) return NULL;
    }

    if (*value == ']') return value + 1;
    return NULL;
}

static const char* parse_object(cJSON* item, const char* value) {
    cJSON* child;
    if (*value != '{') return NULL;

    item->type = cJSON_Object;
    value = skip_whitespace(value + 1);
    if (*value == '}') return value + 1;

    item->child = child = cJSON_New_Item();
    if (!child) return NULL;

    value = skip_whitespace(parse_string(child, skip_whitespace(value)));
    if (!value) return NULL;
    child->string = child->valuestring;
    child->valuestring = NULL;

    if (*value != ':') return NULL;
    value = skip_whitespace(parse_value(child, skip_whitespace(value + 1)));
    if (!value) return NULL;

    while (*value == ',') {
        cJSON* new_item = cJSON_New_Item();
        if (!new_item) return NULL;
        child->next = new_item;
        new_item->prev = child;
        child = new_item;

        value = skip_whitespace(parse_string(child, skip_whitespace(value + 1)));
        if (!value) return NULL;
        child->string = child->valuestring;
        child->valuestring = NULL;

        if (*value != ':') return NULL;
        value = skip_whitespace(parse_value(child, skip_whitespace(value + 1)));
        if (!value) return NULL;
    }

    if (*value == '}') return value + 1;
    return NULL;
}

static const char* parse_value(cJSON* item, const char* value) {
    if (!value) return NULL;
    if (!strncmp(value, "null", 4)) {
        item->type = cJSON_NULL;
        return value + 4;
    }
    if (!strncmp(value, "false", 5)) {
        item->type = cJSON_False;
        return value + 5;
    }
    if (!strncmp(value, "true", 4)) {
        item->type = cJSON_True;
        item->valueint = 1;
        return value + 4;
    }
    if (*value == '\"') return parse_string(item, value);
    if (*value == '-' || (*value >= '0' && *value <= '9')) return parse_number(item, value);
    if (*value == '[') return parse_array(item, value);
    if (*value == '{') return parse_object(item, value);

    return NULL;
}

static cJSON* cJSON_Parse(const char* value) {
    cJSON* item = cJSON_New_Item();
    if (!item) return NULL;

    if (!parse_value(item, skip_whitespace(value))) {
        cJSON_Delete(item);
        return NULL;
    }
    return item;
}

static cJSON* cJSON_GetObjectItem(cJSON* object, const char* string) {
    cJSON* c = object ? object->child : NULL;
    while (c && c->string && strcmp(c->string, string)) c = c->next;
    return c;
}

static int cJSON_GetArraySize(cJSON* array) {
    cJSON* c = array ? array->child : NULL;
    int i = 0;
    while (c) {
        i++;
        c = c->next;
    }
    return i;
}

static cJSON* cJSON_GetArrayItem(cJSON* array, int item) {
    cJSON* c = array ? array->child : NULL;
    while (c && item > 0) {
        item--;
        c = c->next;
    }
    return c;
}

static bool cJSON_IsBool(cJSON* item) {
    return item && (item->type == cJSON_True || item->type == cJSON_False);
}

static bool cJSON_IsNumber(cJSON* item) {
    return item && (item->type == cJSON_Number);
}

static bool cJSON_IsString(cJSON* item) {
    return item && (item->type == cJSON_String);
}

static bool cJSON_IsArray(cJSON* item) {
    return item && (item->type == cJSON_Array);
}

static bool cJSON_IsObject(cJSON* item) {
    return item && (item->type == cJSON_Object);
}

BlurConfig* config_create(void) {
    BlurConfig* config = (BlurConfig*)calloc(1, sizeof(BlurConfig));
    if (!config) return NULL;

    config->blur = true;
    config->blur_amount = 1.0f;
    strcpy(config->blur_output_fps, "60");
    strcpy(config->blur_weighting, "gaussian_sym");
    config->custom_weights = NULL;
    config->custom_weights_count = 0;

    config->interpolate = true;
    strcpy(config->interpolated_fps, "5x");
    strcpy(config->interpolation_method, "rife");
    config->interpolation_block_size = 16;
    config->interpolation_mask_area = 0.0f;
    config->pre_interpolation = false;
    strcpy(config->pre_interpolated_fps, "2x");

    config->quality = 20;
    config->deduplicate = false;
    config->deduplicate_range = 5;
    config->deduplicate_threshold = 0.2f;

    config->gpu_decoding = false;
    config->gpu_interpolation = false;
    config->gpu_encoding = false;
    strcpy(config->gpu_type, "nvidia");

    config->manual_svp = false;
    strcpy(config->svp_super_string, "{pel:2,gpu:1}");
    strcpy(config->svp_vectors_string, "{block:{w:32,h:32},main:{search:{coarse:{distance:-8}}}}");
    strcpy(config->svp_smooth_string, "{rate:{num:5,den:1},algo:13,mask:{area:100}}");
    strcpy(config->svp_preset, "default");
    config->svp_algorithm = 13;

    config->brightness = 0.0f;
    config->saturation = 0.0f;
    config->contrast = 0.0f;
    config->gamma = 1.0f;

    strcpy(config->container, "mp4");
    strcpy(config->codec, "h264");
    config->bitrate = 0;
    strcpy(config->pixel_format, "yuv420p");

    config->threads = 0;
    config->verbose = false;
    config->debug = false;
    config->timescale = 1.0f;
    config->pitch_correction = true;
    config->ffmpeg_filters[0] = '\0';

    return config;
}

void config_destroy(BlurConfig* config) {
    if (!config) return;

    if (config->custom_weights) {
        free(config->custom_weights);
    }

    free(config);
}

static bool load_json_string(cJSON* json, const char* key, char* dest, size_t size) {
    cJSON* item = cJSON_GetObjectItem(json, key);
    if (cJSON_IsString(item) && item->valuestring) {
        strncpy(dest, item->valuestring, size - 1);
        dest[size - 1] = '\0';
        return true;
    }
    return false;
}

static bool load_json_bool(cJSON* json, const char* key, bool* dest) {
    cJSON* item = cJSON_GetObjectItem(json, key);
    if (cJSON_IsBool(item)) {
        *dest = (item->type == cJSON_True);
        return true;
    }
    return false;
}

static bool load_json_float(cJSON* json, const char* key, float* dest) {
    cJSON* item = cJSON_GetObjectItem(json, key);
    if (cJSON_IsNumber(item)) {
        *dest = (float)item->valuedouble;
        return true;
    }
    return false;
}

static bool load_json_int(cJSON* json, const char* key, int* dest) {
    cJSON* item = cJSON_GetObjectItem(json, key);
    if (cJSON_IsNumber(item)) {
        *dest = item->valueint;
        return true;
    }
    return false;
}

bool config_load_file(BlurConfig* config, const char* filename) {
    FILE* fp = fopen(filename, "rb");
    if (!fp) {
        fprintf(stderr, "Error: Cannot open config file: %s\n", filename);
        return false;
    }

    fseek(fp, 0, SEEK_END);
    long size = ftell(fp);
    fseek(fp, 0, SEEK_SET);

    if (size <= 0) {
        fprintf(stderr, "Error: Config file is empty or invalid: %s\n", filename);
        fclose(fp);
        return false;
    }

    char* buffer = (char*)malloc(size + 1);
    if (!buffer) {
        fprintf(stderr, "Error: Failed to allocate memory for config file\n");
        fclose(fp);
        return false;
    }

    size_t read_size = fread(buffer, 1, size, fp);
    buffer[read_size] = '\0';
    fclose(fp);

    cJSON* json = cJSON_Parse(buffer);
    free(buffer);

    if (!json) {
        fprintf(stderr, "Error: Invalid JSON in config file: %s\n", filename);
        return false;
    }

    load_json_bool(json, "blur", &config->blur);
    load_json_float(json, "blur_amount", &config->blur_amount);
    load_json_string(json, "blur_output_fps", config->blur_output_fps, sizeof(config->blur_output_fps));
    load_json_string(json, "blur_weighting", config->blur_weighting, sizeof(config->blur_weighting));

    cJSON* weights_array = cJSON_GetObjectItem(json, "custom_weights");
    if (cJSON_IsArray(weights_array)) {
        int count = cJSON_GetArraySize(weights_array);
        if (count > 0) {
            config->custom_weights = (float*)malloc(count * sizeof(float));
            if (config->custom_weights) {
                config->custom_weights_count = count;
                for (int i = 0; i < count; i++) {
                    cJSON* item = cJSON_GetArrayItem(weights_array, i);
                    if (cJSON_IsNumber(item)) {
                        config->custom_weights[i] = (float)item->valuedouble;
                    }
                    else {
                        config->custom_weights[i] = 1.0f / count;
                    }
                }
            }
        }
    }

    load_json_bool(json, "interpolate", &config->interpolate);
    load_json_string(json, "interpolated_fps", config->interpolated_fps, sizeof(config->interpolated_fps));
    load_json_string(json, "interpolation_method", config->interpolation_method, sizeof(config->interpolation_method));
    load_json_int(json, "interpolation_block_size", &config->interpolation_block_size);
    load_json_float(json, "interpolation_mask_area", &config->interpolation_mask_area);
    load_json_bool(json, "pre_interpolation", &config->pre_interpolation);
    load_json_string(json, "pre_interpolated_fps", config->pre_interpolated_fps, sizeof(config->pre_interpolated_fps));

    load_json_int(json, "quality", &config->quality);
    load_json_bool(json, "deduplicate", &config->deduplicate);
    load_json_int(json, "deduplicate_range", &config->deduplicate_range);
    load_json_float(json, "deduplicate_threshold", &config->deduplicate_threshold);

    load_json_bool(json, "gpu_decoding", &config->gpu_decoding);
    load_json_bool(json, "gpu_interpolation", &config->gpu_interpolation);
    load_json_bool(json, "gpu_encoding", &config->gpu_encoding);
    load_json_string(json, "gpu_type", config->gpu_type, sizeof(config->gpu_type));

    load_json_bool(json, "manual_svp", &config->manual_svp);
    load_json_string(json, "svp_super_string", config->svp_super_string, sizeof(config->svp_super_string));
    load_json_string(json, "svp_vectors_string", config->svp_vectors_string, sizeof(config->svp_vectors_string));
    load_json_string(json, "svp_smooth_string", config->svp_smooth_string, sizeof(config->svp_smooth_string));
    load_json_string(json, "svp_preset", config->svp_preset, sizeof(config->svp_preset));
    load_json_int(json, "svp_algorithm", &config->svp_algorithm);

    load_json_float(json, "brightness", &config->brightness);
    load_json_float(json, "saturation", &config->saturation);
    load_json_float(json, "contrast", &config->contrast);
    load_json_float(json, "gamma", &config->gamma);

    load_json_string(json, "container", config->container, sizeof(config->container));
    load_json_string(json, "codec", config->codec, sizeof(config->codec));
    load_json_int(json, "bitrate", &config->bitrate);
    load_json_string(json, "pixel_format", config->pixel_format, sizeof(config->pixel_format));

    load_json_int(json, "threads", &config->threads);
    load_json_bool(json, "verbose", &config->verbose);
    load_json_bool(json, "debug", &config->debug);
    load_json_float(json, "timescale", &config->timescale);
    load_json_bool(json, "pitch_correction", &config->pitch_correction);
    load_json_string(json, "ffmpeg_filters", config->ffmpeg_filters, sizeof(config->ffmpeg_filters));

    cJSON_Delete(json);
    return true;
}

#ifdef _WIN32
static int optind = 1;
static char* optarg = NULL;

static int getopt_long(int argc, char* const argv[], const char* optstring,
    const struct option* longopts, int* longindex) {

    if (optind >= argc) return -1;

    char* arg = argv[optind];
    if (arg[0] != '-') return -1;

    optind++;

    if (arg[1] == '-') {
        for (int i = 0; longopts && longopts[i].name; i++) {
            if (strcmp(arg + 2, longopts[i].name) == 0) {
                if (longindex) *longindex = i;
                if (longopts[i].has_arg) {
                    if (optind < argc) {
                        optarg = argv[optind++];
                    }
                    else {
                        return '?';
                    }
                }
                return longopts[i].val;
            }
        }
        return '?';
    }

    char c = arg[1];
    char* pos = strchr(optstring, c);
    if (!pos) return '?';

    if (pos[1] == ':') {
        if (optind < argc) {
            optarg = argv[optind++];
        }
        else {
            return '?';
        }
    }

    return c;
}
#endif

static void apply_preset(BlurConfig* config, const char* preset) {
    if (strcmp(preset, "gaming") == 0) {
        config->blur_amount = 0.3f;
        strcpy(config->blur_weighting, "gaussian_sym");
        config->interpolate = true;
        strcpy(config->interpolated_fps, "5x");
        config->interpolation_block_size = 16;
        config->deduplicate = true;
        config->deduplicate_threshold = 0.1f;
    }
    else if (strcmp(preset, "cinematic") == 0) {
        config->blur_amount = 1.0f;
        strcpy(config->blur_weighting, "gaussian");
        config->interpolate = true;
        strcpy(config->interpolated_fps, "3x");
        config->interpolation_block_size = 32;
        config->deduplicate = false;
    }
    else if (strcmp(preset, "smooth") == 0) {
        config->blur_amount = 1.5f;
        strcpy(config->blur_weighting, "gaussian_sym");
        config->interpolate = true;
        strcpy(config->interpolated_fps, "10x");
        config->interpolation_block_size = 8;
        config->deduplicate = true;
        config->deduplicate_threshold = 0.05f;
    }
    else if (strcmp(preset, "fast") == 0) {
        config->blur_amount = 0.5f;
        strcpy(config->blur_weighting, "equal");
        config->interpolate = false;
        config->deduplicate = true;
        config->threads = 0;
    }
    else if (strcmp(preset, "quality") == 0) {
        config->blur_amount = 1.2f;
        strcpy(config->blur_weighting, "gaussian_sym");
        config->interpolate = true;
        strcpy(config->interpolated_fps, "8x");
        config->pre_interpolation = true;
        config->quality = 18;
    }
}

bool config_parse_args(BlurConfig* config, int argc, char* argv[]) {
    static struct option long_options[] = {
        {"help", no_argument, 0, 'h'},
        {"version", no_argument, 0, 0},
        {"output", required_argument, 0, 'o'},
        {"config", required_argument, 0, 'c'},
        {"blur-amount", required_argument, 0, 0},
        {"blur-output-fps", required_argument, 0, 0},
        {"blur-weighting", required_argument, 0, 0},
        {"custom-weights", required_argument, 0, 0},
        {"interpolate", no_argument, 0, 0},
        {"no-interpolate", no_argument, 0, 0},
        {"interpolated-fps", required_argument, 0, 0},
        {"interpolation-method", required_argument, 0, 0},
        {"interpolation-block-size", required_argument, 0, 0},
        {"interpolation-mask-area", required_argument, 0, 0},
        {"pre-interpolation", no_argument, 0, 0},
        {"pre-interpolated-fps", required_argument, 0, 0},
        {"gpu", no_argument, 0, 0},
        {"gpu-decoding", no_argument, 0, 0},
        {"gpu-interpolation", no_argument, 0, 0},
        {"gpu-encoding", no_argument, 0, 0},
        {"gpu-type", required_argument, 0, 0},
        {"quality", required_argument, 0, 0},
        {"deduplicate", no_argument, 0, 0},
        {"deduplicate-range", required_argument, 0, 0},
        {"deduplicate-threshold", required_argument, 0, 0},
        {"preset", required_argument, 0, 0},
        {"verbose", no_argument, 0, 'v'},
        {"debug", no_argument, 0, 0},
        {"threads", required_argument, 0, 0},
        {"container", required_argument, 0, 0},
        {"codec", required_argument, 0, 0},
        {"bitrate", required_argument, 0, 0},
        {"pixel-format", required_argument, 0, 0},
        {"brightness", required_argument, 0, 0},
        {"saturation", required_argument, 0, 0},
        {"contrast", required_argument, 0, 0},
        {"gamma", required_argument, 0, 0},
        {"timescale", required_argument, 0, 0},
        {"pitch-correction", no_argument, 0, 0},
        {"no-pitch-correction", no_argument, 0, 0},
        {"ffmpeg-filters", required_argument, 0, 0},
        {"manual-svp", no_argument, 0, 0},
        {"svp-super", required_argument, 0, 0},
        {"svp-vectors", required_argument, 0, 0},
        {"svp-smooth", required_argument, 0, 0},
        {"svp-preset", required_argument, 0, 0},
        {"svp-algorithm", required_argument, 0, 0},
        {0, 0, 0, 0}
    };

    int option_index = 0;
    int c;

#ifdef _WIN32
    optind = 1;
    optarg = NULL;
#endif

    while ((c = getopt_long(argc, argv, "ho:c:v", long_options, &option_index)) != -1) {
        switch (c) {
        case 'h':
            return false;

        case 'o':
            if (optarg) {
                strncpy(config->output_file, optarg, sizeof(config->output_file) - 1);
                config->output_file[sizeof(config->output_file) - 1] = '\0';
            }
            break;

        case 'c':
            break;

        case 'v':
            config->verbose = true;
            break;

        case 0:
            if (strcmp(long_options[option_index].name, "version") == 0) {
                return false;
            }
            else if (strcmp(long_options[option_index].name, "blur-amount") == 0) {
                if (optarg) config->blur_amount = (float)atof(optarg);
            }
            else if (strcmp(long_options[option_index].name, "blur-output-fps") == 0) {
                if (optarg) {
                    strncpy(config->blur_output_fps, optarg, sizeof(config->blur_output_fps) - 1);
                    config->blur_output_fps[sizeof(config->blur_output_fps) - 1] = '\0';
                }
            }
            else if (strcmp(long_options[option_index].name, "blur-weighting") == 0) {
                if (optarg) {
                    strncpy(config->blur_weighting, optarg, sizeof(config->blur_weighting) - 1);
                    config->blur_weighting[sizeof(config->blur_weighting) - 1] = '\0';
                }
            }
            else if (strcmp(long_options[option_index].name, "custom-weights") == 0) {
                if (optarg) {
                    char* weights_str = strdup(optarg);
                    char* token = strtok(weights_str, ",");
                    int count = 0;

                    while (token && count < 64) {
                        count++;
                        token = strtok(NULL, ",");
                    }

                    if (count > 0) {
                        free(weights_str);
                        weights_str = strdup(optarg);
                        config->custom_weights = (float*)malloc(count * sizeof(float));
                        config->custom_weights_count = count;

                        token = strtok(weights_str, ",");
                        for (int i = 0; i < count && token; i++) {
                            config->custom_weights[i] = (float)atof(token);
                            token = strtok(NULL, ",");
                        }
                        strcpy(config->blur_weighting, "custom");
                    }
                    free(weights_str);
                }
            }
            else if (strcmp(long_options[option_index].name, "interpolate") == 0) {
                config->interpolate = true;
            }
            else if (strcmp(long_options[option_index].name, "no-interpolate") == 0) {
                config->interpolate = false;
            }
            else if (strcmp(long_options[option_index].name, "interpolated-fps") == 0) {
                if (optarg) {
                    strncpy(config->interpolated_fps, optarg, sizeof(config->interpolated_fps) - 1);
                    config->interpolated_fps[sizeof(config->interpolated_fps) - 1] = '\0';
                }
            }
            else if (strcmp(long_options[option_index].name, "interpolation-method") == 0) {
                if (optarg) {
                    strncpy(config->interpolation_method, optarg, sizeof(config->interpolation_method) - 1);
                    config->interpolation_method[sizeof(config->interpolation_method) - 1] = '\0';
                }
            }
            else if (strcmp(long_options[option_index].name, "interpolation-block-size") == 0) {
                if (optarg) config->interpolation_block_size = atoi(optarg);
            }
            else if (strcmp(long_options[option_index].name, "interpolation-mask-area") == 0) {
                if (optarg) config->interpolation_mask_area = (float)atof(optarg);
            }
            else if (strcmp(long_options[option_index].name, "pre-interpolation") == 0) {
                config->pre_interpolation = true;
            }
            else if (strcmp(long_options[option_index].name, "pre-interpolated-fps") == 0) {
                if (optarg) {
                    strncpy(config->pre_interpolated_fps, optarg, sizeof(config->pre_interpolated_fps) - 1);
                    config->pre_interpolated_fps[sizeof(config->pre_interpolated_fps) - 1] = '\0';
                }
            }
            else if (strcmp(long_options[option_index].name, "gpu") == 0) {
                config->gpu_decoding = true;
                config->gpu_interpolation = true;
                config->gpu_encoding = true;
            }
            else if (strcmp(long_options[option_index].name, "gpu-decoding") == 0) {
                config->gpu_decoding = true;
            }
            else if (strcmp(long_options[option_index].name, "gpu-interpolation") == 0) {
                config->gpu_interpolation = true;
            }
            else if (strcmp(long_options[option_index].name, "gpu-encoding") == 0) {
                config->gpu_encoding = true;
            }
            else if (strcmp(long_options[option_index].name, "gpu-type") == 0) {
                if (optarg) {
                    strncpy(config->gpu_type, optarg, sizeof(config->gpu_type) - 1);
                    config->gpu_type[sizeof(config->gpu_type) - 1] = '\0';
                }
            }
            else if (strcmp(long_options[option_index].name, "quality") == 0) {
                if (optarg) config->quality = atoi(optarg);
            }
            else if (strcmp(long_options[option_index].name, "deduplicate") == 0) {
                config->deduplicate = true;
            }
            else if (strcmp(long_options[option_index].name, "deduplicate-range") == 0) {
                if (optarg) config->deduplicate_range = atoi(optarg);
            }
            else if (strcmp(long_options[option_index].name, "deduplicate-threshold") == 0) {
                if (optarg) config->deduplicate_threshold = (float)atof(optarg);
            }
            else if (strcmp(long_options[option_index].name, "preset") == 0) {
                if (optarg) apply_preset(config, optarg);
            }
            else if (strcmp(long_options[option_index].name, "debug") == 0) {
                config->debug = true;
                config->verbose = true;
            }
            else if (strcmp(long_options[option_index].name, "threads") == 0) {
                if (optarg) config->threads = atoi(optarg);
            }
            else if (strcmp(long_options[option_index].name, "container") == 0) {
                if (optarg) {
                    strncpy(config->container, optarg, sizeof(config->container) - 1);
                    config->container[sizeof(config->container) - 1] = '\0';
                }
            }
            else if (strcmp(long_options[option_index].name, "codec") == 0) {
                if (optarg) {
                    strncpy(config->codec, optarg, sizeof(config->codec) - 1);
                    config->codec[sizeof(config->codec) - 1] = '\0';
                }
            }
            else if (strcmp(long_options[option_index].name, "bitrate") == 0) {
                if (optarg) config->bitrate = atoi(optarg);
            }
            else if (strcmp(long_options[option_index].name, "pixel-format") == 0) {
                if (optarg) {
                    strncpy(config->pixel_format, optarg, sizeof(config->pixel_format) - 1);
                    config->pixel_format[sizeof(config->pixel_format) - 1] = '\0';
                }
            }
            else if (strcmp(long_options[option_index].name, "brightness") == 0) {
                if (optarg) config->brightness = (float)atof(optarg);
            }
            else if (strcmp(long_options[option_index].name, "saturation") == 0) {
                if (optarg) config->saturation = (float)atof(optarg);
            }
            else if (strcmp(long_options[option_index].name, "contrast") == 0) {
                if (optarg) config->contrast = (float)atof(optarg);
            }
            else if (strcmp(long_options[option_index].name, "gamma") == 0) {
                if (optarg) config->gamma = (float)atof(optarg);
            }
            else if (strcmp(long_options[option_index].name, "timescale") == 0) {
                if (optarg) config->timescale = (float)atof(optarg);
            }
            else if (strcmp(long_options[option_index].name, "pitch-correction") == 0) {
                config->pitch_correction = true;
            }
            else if (strcmp(long_options[option_index].name, "no-pitch-correction") == 0) {
                config->pitch_correction = false;
            }
            else if (strcmp(long_options[option_index].name, "ffmpeg-filters") == 0) {
                if (optarg) {
                    strncpy(config->ffmpeg_filters, optarg, sizeof(config->ffmpeg_filters) - 1);
                    config->ffmpeg_filters[sizeof(config->ffmpeg_filters) - 1] = '\0';
                }
            }
            else if (strcmp(long_options[option_index].name, "manual-svp") == 0) {
                config->manual_svp = true;
            }
            else if (strcmp(long_options[option_index].name, "svp-super") == 0) {
                if (optarg) {
                    strncpy(config->svp_super_string, optarg, sizeof(config->svp_super_string) - 1);
                    config->svp_super_string[sizeof(config->svp_super_string) - 1] = '\0';
                    config->manual_svp = true;
                }
            }
            else if (strcmp(long_options[option_index].name, "svp-vectors") == 0) {
                if (optarg) {
                    strncpy(config->svp_vectors_string, optarg, sizeof(config->svp_vectors_string) - 1);
                    config->svp_vectors_string[sizeof(config->svp_vectors_string) - 1] = '\0';
                    config->manual_svp = true;
                }
            }
            else if (strcmp(long_options[option_index].name, "svp-smooth") == 0) {
                if (optarg) {
                    strncpy(config->svp_smooth_string, optarg, sizeof(config->svp_smooth_string) - 1);
                    config->svp_smooth_string[sizeof(config->svp_smooth_string) - 1] = '\0';
                    config->manual_svp = true;
                }
            }
            else if (strcmp(long_options[option_index].name, "svp-preset") == 0) {
                if (optarg) {
                    strncpy(config->svp_preset, optarg, sizeof(config->svp_preset) - 1);
                    config->svp_preset[sizeof(config->svp_preset) - 1] = '\0';
                }
            }
            else if (strcmp(long_options[option_index].name, "svp-algorithm") == 0) {
                if (optarg) config->svp_algorithm = atoi(optarg);
            }
            break;

        default:
            return false;
        }
    }

    if (optind < argc) {
        strncpy(config->input_file, argv[optind], sizeof(config->input_file) - 1);
        config->input_file[sizeof(config->input_file) - 1] = '\0';
    }
    else {
        fprintf(stderr, "Error: No input file specified\n");
        return false;
    }

    return true;
}

void config_print(const BlurConfig* config) {
    printf("Motion Blur Configuration:\n");
    printf("=========================\n");
    printf("Input file: %s\n", config->input_file);
    printf("Output file: %s\n", config->output_file);
    printf("\n");

    printf("Blur Settings:\n");
    printf("  Enabled: %s\n", config->blur ? "yes" : "no");
    printf("  Amount: %.2f\n", config->blur_amount);
    printf("  Output FPS: %s\n", config->blur_output_fps);
    printf("  Weighting: %s\n", config->blur_weighting);
    if (config->custom_weights && config->custom_weights_count > 0) {
        printf("  Custom weights (%d): ", config->custom_weights_count);
        for (int i = 0; i < config->custom_weights_count; i++) {
            printf("%.3f ", config->custom_weights[i]);
        }
        printf("\n");
    }
    printf("\n");

    printf("Interpolation Settings:\n");
    printf("  Enabled: %s\n", config->interpolate ? "yes" : "no");
    if (config->interpolate) {
        printf("  Target FPS: %s\n", config->interpolated_fps);
        printf("  Method: %s\n", config->interpolation_method);
        printf("  Block size: %d\n", config->interpolation_block_size);
        printf("  Mask area: %.2f\n", config->interpolation_mask_area);
        printf("  Pre-interpolation: %s\n", config->pre_interpolation ? "yes" : "no");
        if (config->pre_interpolation) {
            printf("  Pre-interpolated FPS: %s\n", config->pre_interpolated_fps);
        }
    }
    printf("\n");

    printf("Quality Settings:\n");
    printf("  CRF/QP: %d\n", config->quality);
    printf("  Container: %s\n", config->container);
    printf("  Codec: %s\n", config->codec);
    if (config->bitrate > 0) {
        printf("  Bitrate: %d kbps\n", config->bitrate);
    }
    printf("  Pixel format: %s\n", config->pixel_format);
    printf("\n");

    printf("GPU Acceleration:\n");
    printf("  Decoding: %s\n", config->gpu_decoding ? "yes" : "no");
    printf("  Interpolation: %s\n", config->gpu_interpolation ? "yes" : "no");
    printf("  Encoding: %s\n", config->gpu_encoding ? "yes" : "no");
    printf("  Type: %s\n", config->gpu_type);
    printf("\n");

    if (config->deduplicate) {
        printf("Deduplication:\n");
        printf("  Enabled: yes\n");
        printf("  Range: %d frames\n", config->deduplicate_range);
        printf("  Threshold: %.3f\n", config->deduplicate_threshold);
        printf("\n");
    }

    if (config->brightness != 0 || config->saturation != 0 ||
        config->contrast != 0 || config->gamma != 1.0) {
        printf("Color Correction:\n");
        printf("  Brightness: %.2f\n", config->brightness);
        printf("  Saturation: %.2f\n", config->saturation);
        printf("  Contrast: %.2f\n", config->contrast);
        printf("  Gamma: %.2f\n", config->gamma);
        printf("\n");
    }

    if (config->timescale != 1.0) {
        printf("Timing:\n");
        printf("  Timescale: %.2f\n", config->timescale);
        printf("  Pitch correction: %s\n", config->pitch_correction ? "yes" : "no");
        printf("\n");
    }

    if (strlen(config->ffmpeg_filters) > 0) {
        printf("Custom Filters:\n");
        printf("  FFmpeg filters: %s\n", config->ffmpeg_filters);
        printf("\n");
    }

    printf("Processing:\n");
    printf("  Threads: %d%s\n", config->threads,
        config->threads == 0 ? " (auto)" : "");
    printf("  Verbose: %s\n", config->verbose ? "yes" : "no");
    printf("  Debug: %s\n", config->debug ? "yes" : "no");
    printf("\n");
}

bool config_validate(const BlurConfig* config) {
    if (strlen(config->input_file) == 0) {
        fprintf(stderr, "Error: Input file not specified\n");
        return false;
    }

    if (access(config->input_file, 0) != 0) {
        fprintf(stderr, "Error: Input file does not exist: %s\n", config->input_file);
        return false;
    }

    if (strlen(config->output_file) == 0) {
        fprintf(stderr, "Error: Output file not specified\n");
        return false;
    }

    if (config->blur_amount < 0 || config->blur_amount > 10) {
        fprintf(stderr, "Error: Blur amount must be between 0 and 10\n");
        return false;
    }

    if (config->quality < 0 || config->quality > 51) {
        fprintf(stderr, "Error: Quality (CRF) must be between 0 and 51\n");
        return false;
    }

    if (config->interpolation_block_size != 4 && config->interpolation_block_size != 8 &&
        config->interpolation_block_size != 16 && config->interpolation_block_size != 32) {
        fprintf(stderr, "Error: Interpolation block size must be 4, 8, 16, or 32\n");
        return false;
    }

    if (config->interpolation_mask_area < 0 || config->interpolation_mask_area > 1) {
        fprintf(stderr, "Error: Interpolation mask area must be between 0 and 1\n");
        return false;
    }

    if (config->brightness < -1 || config->brightness > 1) {
        fprintf(stderr, "Error: Brightness must be between -1 and 1\n");
        return false;
    }

    if (config->saturation < -1 || config->saturation > 1) {
        fprintf(stderr, "Error: Saturation must be between -1 and 1\n");
        return false;
    }

    if (config->contrast < -1 || config->contrast > 1) {
        fprintf(stderr, "Error: Contrast must be between -1 and 1\n");
        return false;
    }

    if (config->gamma < 0.1 || config->gamma > 10) {
        fprintf(stderr, "Error: Gamma must be between 0.1 and 10\n");
        return false;
    }

    if (config->timescale <= 0 || config->timescale > 100) {
        fprintf(stderr, "Error: Timescale must be between 0 and 100\n");
        return false;
    }

    if (config->threads < 0 || config->threads > 256) {
        fprintf(stderr, "Error: Thread count must be between 0 and 256\n");
        return false;
    }

    const char* valid_weightings[] = {
        "equal", "gaussian_sym", "gaussian", "vegas", "pyramid",
        "ascending", "descending", "gaussian_reverse", "custom"
    };
    bool valid_weighting = false;
    for (int i = 0; i < 9; i++) {
        if (strcmp(config->blur_weighting, valid_weightings[i]) == 0) {
            valid_weighting = true;
            break;
        }
    }
    if (!valid_weighting) {
        fprintf(stderr, "Error: Invalid blur weighting: %s\n", config->blur_weighting);
        return false;
    }

    if (strcmp(config->blur_weighting, "custom") == 0 &&
        (!config->custom_weights || config->custom_weights_count <= 0)) {
        fprintf(stderr, "Error: Custom weighting selected but no weights provided\n");
        return false;
    }

    if (strcmp(config->interpolation_method, "rife") != 0 &&
        strcmp(config->interpolation_method, "svp") != 0) {
        fprintf(stderr, "Error: Invalid interpolation method: %s (must be 'rife' or 'svp')\n",
            config->interpolation_method);
        return false;
    }

    if (strcmp(config->container, "mp4") != 0 &&
        strcmp(config->container, "mkv") != 0 &&
        strcmp(config->container, "avi") != 0 &&
        strcmp(config->container, "mov") != 0) {
        fprintf(stderr, "Error: Invalid container format: %s\n", config->container);
        return false;
    }

    if (strcmp(config->codec, "h264") != 0 &&
        strcmp(config->codec, "h265") != 0 &&
        strcmp(config->codec, "hevc") != 0 &&
        strcmp(config->codec, "av1") != 0 &&
        strcmp(config->codec, "vp9") != 0) {
        fprintf(stderr, "Error: Invalid codec: %s\n", config->codec);
        return false;
    }

    if (strcmp(config->gpu_type, "nvidia") != 0 &&
        strcmp(config->gpu_type, "amd") != 0 &&
        strcmp(config->gpu_type, "intel") != 0) {
        fprintf(stderr, "Error: Invalid GPU type: %s (must be nvidia, amd, or intel)\n",
            config->gpu_type);
        return false;
    }

    return true;
}

static float* generate_equal_weights(int count) {
    float* weights = (float*)malloc(count * sizeof(float));
    if (!weights) return NULL;

    float weight = 1.0f / count;
    for (int i = 0; i < count; i++) {
        weights[i] = weight;
    }

    return weights;
}

static float* generate_gaussian_weights(int count, bool symmetric, bool reverse) {
    float* weights = (float*)malloc(count * sizeof(float));
    if (!weights) return NULL;

    float sigma = count / 6.0f;
    float center = symmetric ? (count - 1) / 2.0f : 0.0f;
    float sum = 0.0f;

    for (int i = 0; i < count; i++) {
        float x = i - center;
        weights[i] = expf(-(x * x) / (2 * sigma * sigma));
        if (reverse) weights[i] = 1.0f - weights[i];
        sum += weights[i];
    }

    if (sum > 0) {
        for (int i = 0; i < count; i++) {
            weights[i] /= sum;
        }
    }

    return weights;
}

static float* generate_vegas_weights(int count) {
    float* weights = (float*)malloc(count * sizeof(float));
    if (!weights) return NULL;

    float sum = 0.0f;
    for (int i = 0; i < count; i++) {
        float t = (float)i / (count - 1);
        weights[i] = 1.0f - fabsf(2.0f * t - 1.0f);
        sum += weights[i];
    }

    if (sum > 0) {
        for (int i = 0; i < count; i++) {
            weights[i] /= sum;
        }
    }

    return weights;
}

static float* generate_pyramid_weights(int count) {
    float* weights = (float*)malloc(count * sizeof(float));
    if (!weights) return NULL;

    int half = count / 2;
    float sum = 0.0f;

    for (int i = 0; i < count; i++) {
        if (i <= half) {
            weights[i] = (float)(i + 1);
        }
        else {
            weights[i] = (float)(count - i);
        }
        sum += weights[i];
    }

    if (sum > 0) {
        for (int i = 0; i < count; i++) {
            weights[i] /= sum;
        }
    }

    return weights;
}

static float* generate_linear_weights(int count, bool ascending) {
    float* weights = (float*)malloc(count * sizeof(float));
    if (!weights) return NULL;

    float sum = 0.0f;
    for (int i = 0; i < count; i++) {
        weights[i] = ascending ? (float)(i + 1) : (float)(count - i);
        sum += weights[i];
    }

    if (sum > 0) {
        for (int i = 0; i < count; i++) {
            weights[i] /= sum;
        }
    }

    return weights;
}

float* config_get_weights(const BlurConfig* config, int frame_count, int* weight_count) {
    if (!config || frame_count <= 0 || !weight_count) {
        return NULL;
    }

    *weight_count = frame_count;

    if (config->custom_weights && config->custom_weights_count == frame_count) {
        float* weights = (float*)malloc(frame_count * sizeof(float));
        if (!weights) return NULL;

        float sum = 0.0f;
        for (int i = 0; i < frame_count; i++) {
            weights[i] = config->custom_weights[i];
            sum += weights[i];
        }

        if (sum > 0) {
            for (int i = 0; i < frame_count; i++) {
                weights[i] /= sum;
            }
        }

        return weights;
    }

    if (strcmp(config->blur_weighting, "equal") == 0) {
        return generate_equal_weights(frame_count);
    }
    else if (strcmp(config->blur_weighting, "gaussian_sym") == 0) {
        return generate_gaussian_weights(frame_count, true, false);
    }
    else if (strcmp(config->blur_weighting, "gaussian") == 0) {
        return generate_gaussian_weights(frame_count, false, false);
    }
    else if (strcmp(config->blur_weighting, "gaussian_reverse") == 0) {
        return generate_gaussian_weights(frame_count, true, true);
    }
    else if (strcmp(config->blur_weighting, "vegas") == 0) {
        return generate_vegas_weights(frame_count);
    }
    else if (strcmp(config->blur_weighting, "pyramid") == 0) {
        return generate_pyramid_weights(frame_count);
    }
    else if (strcmp(config->blur_weighting, "ascending") == 0) {
        return generate_linear_weights(frame_count, true);
    }
    else if (strcmp(config->blur_weighting, "descending") == 0) {
        return generate_linear_weights(frame_count, false);
    }

    return generate_equal_weights(frame_count);
}