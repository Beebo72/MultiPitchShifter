#include <ctype.h>
#include <errno.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#ifdef USE_EMBEDDED_RUBBERBAND
#include "import/rubberband-src/rubberband/rubberband-c.h"
#endif
#ifdef USE_EMBEDDED_SIGNALSMITH
#include "signalsmith_bridge.h"
#endif
#ifdef USE_EMBEDDED_BUNGEE
#include "bungee_bridge.h"
#endif
#ifdef USE_EMBEDDED_SOUNDTOUCH
#include "soundtouch_bridge.h"
#endif

enum PitchBackend {
    BACKEND_RUBBERBAND = 0,
    BACKEND_SIGNALSMITH = 1,
    BACKEND_BUNGEE = 2,
    BACKEND_SOUNDTOUCH = 3,
    BACKEND_BASIC = 4
};

typedef struct {
    uint16_t audio_format;
    uint16_t channels;
    uint32_t sample_rate;
    uint16_t bits_per_sample;
    uint32_t data_size;
    int16_t *samples;
    size_t frame_count;
} Wav;

typedef struct {
    int engine_finer;
    int pitch_option;
    int window_option;
    int transients_option;
    int detector_option;
    int phase_independent;
    int channels_together;
    char cli_args[512];
} RubberBandArgs;

static void init_rubberband_args(RubberBandArgs *args) {
    args->engine_finer = 1;
    args->pitch_option = 1;
    args->window_option = 0;
    args->transients_option = 0;
    args->detector_option = 0;
    args->phase_independent = 0;
    args->channels_together = 1;
    strcpy(args->cli_args, "-3 --pitch-hq");
}

#ifndef USE_EMBEDDED_RUBBERBAND
static int quote_arg(const char *arg, char *out, size_t out_size) {
    size_t pos = 0;

    if (out_size < 3) return 0;
    out[pos++] = '"';
    for (size_t i = 0; arg[i]; i++) {
        if (pos + 3 >= out_size) return 0;
        if (arg[i] == '"') {
            out[pos++] = '\\';
            out[pos++] = '"';
        } else {
            out[pos++] = arg[i];
        }
    }
    out[pos++] = '"';
    out[pos] = '\0';
    return 1;
}

static int has_shell_space(const char *text) {
    for (size_t i = 0; text[i]; i++) {
        if (isspace((unsigned char)text[i])) return 1;
    }
    return 0;
}

static int command_exe_arg(const char *arg, char *out, size_t out_size) {
    if (!has_shell_space(arg) && strchr(arg, '"') == NULL) {
        if (strlen(arg) + 1 > out_size) return 0;
        strcpy(out, arg);
        return 1;
    }

    if (out_size < 6) return 0;
    strcpy(out, "call ");
    return quote_arg(arg, out + 5, out_size - 5);
}

static int file_exists(const char *path) {
    FILE *f = fopen(path, "rb");

    if (!f) return 0;
    fclose(f);
    return 1;
}

static const char *find_rubberband_executable(void) {
    const char *env_path = getenv("RUBBERBAND_EXE");

    if (env_path && env_path[0] && file_exists(env_path)) return env_path;
    if (file_exists("import\\rubberband.exe")) return "import\\rubberband.exe";
    if (file_exists("import/rubberband.exe")) return "import/rubberband.exe";
    if (file_exists("rubberband.exe")) return "rubberband.exe";
    if (file_exists("bin\\rubberband.exe")) return "bin\\rubberband.exe";
    if (file_exists("bin/rubberband.exe")) return "bin/rubberband.exe";
    return "rubberband";
}
#endif

static uint16_t read_u16_le(FILE *f) {
    uint8_t b[2];
    if (fread(b, 1, 2, f) != 2) return 0;
    return (uint16_t)(b[0] | (b[1] << 8));
}

static uint32_t read_u32_le(FILE *f) {
    uint8_t b[4];
    if (fread(b, 1, 4, f) != 4) return 0;
    return (uint32_t)(b[0] | (b[1] << 8) | (b[2] << 16) | (b[3] << 24));
}

static void write_u16_le(FILE *f, uint16_t v) {
    uint8_t b[2] = {(uint8_t)(v & 255), (uint8_t)(v >> 8)};
    fwrite(b, 1, 2, f);
}

static void write_u32_le(FILE *f, uint32_t v) {
    uint8_t b[4] = {
        (uint8_t)(v & 255),
        (uint8_t)((v >> 8) & 255),
        (uint8_t)((v >> 16) & 255),
        (uint8_t)((v >> 24) & 255)
    };
    fwrite(b, 1, 4, f);
}

static int read_exact(FILE *f, char *buf, size_t n) {
    return fread(buf, 1, n, f) == n;
}

static int read_wav(const char *path, Wav *wav) {
    FILE *f = fopen(path, "rb");
    char id[4];
    uint32_t riff_size;
    int found_fmt = 0;
    int found_data = 0;

    memset(wav, 0, sizeof(*wav));
    if (!f) {
        fprintf(stderr, "Could not open input '%s': %s\n", path, strerror(errno));
        return 0;
    }

    if (!read_exact(f, id, 4) || memcmp(id, "RIFF", 4) != 0) goto bad_file;
    riff_size = read_u32_le(f);
    (void)riff_size;
    if (!read_exact(f, id, 4) || memcmp(id, "WAVE", 4) != 0) goto bad_file;

    while (read_exact(f, id, 4)) {
        uint32_t chunk_size = read_u32_le(f);
        long next_chunk = ftell(f) + (long)chunk_size + (chunk_size & 1u);

        if (memcmp(id, "fmt ", 4) == 0) {
            uint32_t byte_rate;
            uint16_t block_align;

            wav->audio_format = read_u16_le(f);
            wav->channels = read_u16_le(f);
            wav->sample_rate = read_u32_le(f);
            byte_rate = read_u32_le(f);
            block_align = read_u16_le(f);
            wav->bits_per_sample = read_u16_le(f);
            (void)byte_rate;
            (void)block_align;
            found_fmt = 1;
        } else if (memcmp(id, "data", 4) == 0) {
            if (!found_fmt) {
                fprintf(stderr, "WAV has data before fmt chunk.\n");
                fclose(f);
                return 0;
            }
            if (wav->audio_format != 1 || wav->bits_per_sample != 16) {
                fprintf(stderr, "Only uncompressed 16-bit PCM WAV files are supported.\n");
                fclose(f);
                return 0;
            }

            wav->data_size = chunk_size;
            wav->frame_count = chunk_size / (wav->channels * sizeof(int16_t));
            wav->samples = (int16_t *)malloc(chunk_size);
            if (!wav->samples) {
                fprintf(stderr, "Out of memory reading audio.\n");
                fclose(f);
                return 0;
            }
            if (fread(wav->samples, 1, chunk_size, f) != chunk_size) {
                fprintf(stderr, "Could not read audio samples.\n");
                free(wav->samples);
                fclose(f);
                return 0;
            }
            found_data = 1;
        }

        if (fseek(f, next_chunk, SEEK_SET) != 0) break;
        if (found_fmt && found_data) break;
    }

    fclose(f);
    if (!found_fmt || !found_data) {
        fprintf(stderr, "Missing fmt or data chunk in WAV file.\n");
        free(wav->samples);
        return 0;
    }
    return 1;

bad_file:
    fprintf(stderr, "Input is not a valid RIFF/WAVE file.\n");
    fclose(f);
    return 0;
}

static int write_wav(const char *path, const Wav *src, const int16_t *samples) {
    FILE *f = fopen(path, "wb");
    uint16_t block_align = (uint16_t)(src->channels * sizeof(int16_t));
    uint32_t byte_rate = src->sample_rate * block_align;
    uint32_t data_size = (uint32_t)(src->frame_count * block_align);
    uint32_t riff_size = 36u + data_size;

    if (!f) {
        fprintf(stderr, "Could not open output '%s': %s\n", path, strerror(errno));
        return 0;
    }

    fwrite("RIFF", 1, 4, f);
    write_u32_le(f, riff_size);
    fwrite("WAVE", 1, 4, f);

    fwrite("fmt ", 1, 4, f);
    write_u32_le(f, 16);
    write_u16_le(f, 1);
    write_u16_le(f, src->channels);
    write_u32_le(f, src->sample_rate);
    write_u32_le(f, byte_rate);
    write_u16_le(f, block_align);
    write_u16_le(f, 16);

    fwrite("data", 1, 4, f);
    write_u32_le(f, data_size);
    fwrite(samples, 1, data_size, f);

    fclose(f);
    return 1;
}

static double sample_wav_linear(const Wav *wav, double frame_pos, int channel) {
    size_t frame0;
    size_t frame1;
    double frac;
    double a;
    double b;

    if (frame_pos < 0.0 || frame_pos >= (double)(wav->frame_count - 1)) {
        return 0.0;
    }

    frame0 = (size_t)frame_pos;
    frame1 = frame0 + 1;
    frac = frame_pos - (double)frame0;
    a = (double)wav->samples[frame0 * wav->channels + channel];
    b = (double)wav->samples[frame1 * wav->channels + channel];
    return a + (b - a) * frac;
}

static double sample_buffer_linear(const double *samples,
                                   size_t frame_count,
                                   uint16_t channels,
                                   double frame_pos,
                                   int channel) {
    size_t frame0;
    size_t frame1;
    double frac;
    double a;
    double b;

    if (frame_pos < 0.0 || frame_pos >= (double)(frame_count - 1)) {
        return 0.0;
    }

    frame0 = (size_t)frame_pos;
    frame1 = frame0 + 1;
    frac = frame_pos - (double)frame0;
    a = samples[frame0 * channels + channel];
    b = samples[frame1 * channels + channel];
    return a + (b - a) * frac;
}

static int parse_pitch_list(const char *text, double **out_pitches, size_t *out_count) {
    char *copy = (char *)malloc(strlen(text) + 1);
    char *token;
    size_t count = 0;
    size_t capacity = 8;
    double *pitches = (double *)malloc(capacity * sizeof(double));

    if (!copy || !pitches) {
        free(copy);
        free(pitches);
        return 0;
    }
    strcpy(copy, text);

    token = strtok(copy, ",");
    while (token) {
        char *end = NULL;
        double value;

        while (isspace((unsigned char)*token)) token++;
        value = strtod(token, &end);
        while (end && isspace((unsigned char)*end)) end++;
        if (!end || *end != '\0') {
            fprintf(stderr, "Invalid pitch value: '%s'\n", token);
            free(copy);
            free(pitches);
            return 0;
        }

        if (count == capacity) {
            double *grown;
            capacity *= 2;
            grown = (double *)realloc(pitches, capacity * sizeof(double));
            if (!grown) {
                free(copy);
                free(pitches);
                return 0;
            }
            pitches = grown;
        }

        pitches[count++] = value;
        token = strtok(NULL, ",");
    }

    free(copy);
    if (count == 0) {
        fprintf(stderr, "Pitch list is empty.\n");
        free(pitches);
        return 0;
    }

    *out_pitches = pitches;
    *out_count = count;
    return 1;
}

static int append_cli_arg(RubberBandArgs *args, const char *arg) {
    size_t current = strlen(args->cli_args);
    size_t add = strlen(arg);

    if (current + add + 2 >= sizeof(args->cli_args)) {
        fprintf(stderr, "Rubber Band args are too long.\n");
        return 0;
    }

    if (current > 0) {
        args->cli_args[current++] = ' ';
        args->cli_args[current] = '\0';
    }
    strcat(args->cli_args, arg);
    return 1;
}

static int apply_rubberband_arg_token(const char *token,
                                      RubberBandArgs *args,
                                      int *preserve_formants) {
    if (strcmp(token, "-2") == 0 ||
        strcmp(token, "--fast") == 0 ||
        strcmp(token, "--faster") == 0 ||
        strcmp(token, "--engine-faster") == 0) {
        args->engine_finer = 0;
        return append_cli_arg(args, "-2");
    }

    if (strcmp(token, "-3") == 0 ||
        strcmp(token, "--fine") == 0 ||
        strcmp(token, "--finer") == 0 ||
        strcmp(token, "--engine-finer") == 0) {
        args->engine_finer = 1;
        return append_cli_arg(args, "-3");
    }

    if (strcmp(token, "-F") == 0 ||
        strcmp(token, "--formant") == 0 ||
        strcmp(token, "--preserve-formants") == 0) {
        *preserve_formants = 1;
        return append_cli_arg(args, "-F");
    }

    if (strcmp(token, "--no-formant") == 0 ||
        strcmp(token, "--formant-shifted") == 0) {
        *preserve_formants = 0;
        return 1;
    }

    if (strcmp(token, "--pitch-speed") == 0) {
        args->pitch_option = 0;
        return append_cli_arg(args, token);
    }

    if (strcmp(token, "--pitch-hq") == 0 ||
        strcmp(token, "--pitch-high-quality") == 0) {
        args->pitch_option = 1;
        return append_cli_arg(args, "--pitch-hq");
    }

    if (strcmp(token, "--pitch-hc") == 0 ||
        strcmp(token, "--pitch-high-consistency") == 0) {
        args->pitch_option = 2;
        return 1;
    }

    if (strcmp(token, "--window-standard") == 0) {
        args->window_option = 0;
        return 1;
    }

    if (strcmp(token, "--window-short") == 0) {
        args->window_option = 1;
        return append_cli_arg(args, token);
    }

    if (strcmp(token, "--window-long") == 0) {
        args->window_option = 2;
        return append_cli_arg(args, token);
    }

    if (strcmp(token, "--transients-crisp") == 0) {
        args->transients_option = 0;
        return 1;
    }

    if (strcmp(token, "--no-transients") == 0) {
        args->transients_option = 0;
        return append_cli_arg(args, "--no-transients");
    }

    if (strcmp(token, "--transients-mixed") == 0 ||
        strcmp(token, "--bl-transients") == 0) {
        args->transients_option = 1;
        return append_cli_arg(args, "--bl-transients");
    }

    if (strcmp(token, "--transients-smooth") == 0 ||
        strcmp(token, "--smoothing") == 0) {
        args->transients_option = 2;
        return append_cli_arg(args, "--smoothing");
    }

    if (strcmp(token, "--detector-compound") == 0) {
        args->detector_option = 0;
        return 1;
    }

    if (strcmp(token, "--detector-percussive") == 0 ||
        strcmp(token, "--detector-perc") == 0) {
        args->detector_option = 1;
        return append_cli_arg(args, "--detector-perc");
    }

    if (strcmp(token, "--detector-soft") == 0) {
        args->detector_option = 2;
        return append_cli_arg(args, "--detector-soft");
    }

    if (strcmp(token, "--phase-laminar") == 0) {
        args->phase_independent = 0;
        return 1;
    }

    if (strcmp(token, "--phase-independent") == 0 ||
        strcmp(token, "--no-lamination") == 0) {
        args->phase_independent = 1;
        return append_cli_arg(args, "--no-lamination");
    }

    if (strcmp(token, "--channels-apart") == 0) {
        args->channels_together = 0;
        return 1;
    }

    if (strcmp(token, "--channels-together") == 0) {
        args->channels_together = 1;
        return 1;
    }

    fprintf(stderr, "Unsupported Rubber Band arg: %s\n", token);
    fprintf(stderr, "Supported examples: -2, -3, -F, --pitch-hq, --pitch-hc, --window-short, --window-long, --transients-smooth, --detector-soft, --phase-independent, --channels-apart.\n");
    return 0;
}

static int parse_rubberband_args(const char *text,
                                 RubberBandArgs *args,
                                 int *preserve_formants) {
    char *copy = (char *)malloc(strlen(text) + 1);
    char *token;

    if (!copy) return 0;
    strcpy(copy, text);

    token = strtok(copy, " \t\r\n,");
    while (token) {
        if (!apply_rubberband_arg_token(token, args, preserve_formants)) {
            free(copy);
            return 0;
        }
        token = strtok(NULL, " \t\r\n,");
    }

    free(copy);
    return 1;
}

static int16_t clamp_i16(double x) {
    if (x > 32767.0) return 32767;
    if (x < -32768.0) return -32768;
    return (int16_t)lrint(x);
}

static double hann_window(size_t i, size_t n) {
    const double pi = 3.14159265358979323846;

    if (n <= 1) return 1.0;
    return 0.5 - 0.5 * cos((2.0 * pi * (double)i) / (double)(n - 1));
}

static double existing_output_sample(const double *output,
                                     const double *weights,
                                     size_t frame,
                                     uint16_t channels,
                                     uint16_t channel) {
    double weight = weights[frame];

    if (weight <= 0.000001) return 0.0;
    return output[frame * channels + channel] / weight;
}

static double overlap_error(const double *input,
                            const double *output,
                            const double *weights,
                            size_t input_frames,
                            size_t output_frames,
                            uint16_t channels,
                            size_t in_pos,
                            size_t out_pos,
                            size_t overlap) {
    double error = 0.0;
    size_t count = 0;

    for (size_t i = 0; i < overlap; i++) {
        size_t in_frame = in_pos + i;
        size_t out_frame = out_pos + i;

        if (in_frame >= input_frames || out_frame >= output_frames) break;
        if (weights[out_frame] <= 0.000001) continue;

        for (uint16_t ch = 0; ch < channels; ch++) {
            double a = input[in_frame * channels + ch];
            double b = existing_output_sample(output, weights, out_frame, channels, ch);
            double d = a - b;

            error += d * d;
            count++;
        }
    }

    if (count == 0) return 0.0;
    return error / (double)count;
}

static size_t find_best_overlap(const double *input,
                                const double *output,
                                const double *weights,
                                size_t input_frames,
                                size_t output_frames,
                                uint16_t channels,
                                size_t nominal_pos,
                                size_t out_pos,
                                size_t frame_size,
                                size_t overlap,
                                size_t search_radius) {
    size_t max_input_pos = input_frames > frame_size ? input_frames - frame_size : 0;
    size_t search_start = nominal_pos > search_radius ? nominal_pos - search_radius : 0;
    size_t search_end = nominal_pos + search_radius;
    size_t best_pos;
    double best_error;

    if (search_end > max_input_pos) search_end = max_input_pos;
    if (search_start > search_end) search_start = search_end;

    best_pos = search_start;
    best_error = overlap_error(input, output, weights, input_frames, output_frames,
                               channels, best_pos, out_pos, overlap);

    for (size_t pos = search_start + 1; pos <= search_end; pos++) {
        double error = overlap_error(input, output, weights, input_frames,
                                     output_frames, channels, pos, out_pos,
                                     overlap);
        if (error < best_error) {
            best_error = error;
            best_pos = pos;
        }
    }

    return best_pos;
}

static int stretch_wsola(const double *input,
                         size_t input_frames,
                         uint16_t channels,
                         size_t output_frames,
                         double *output) {
    size_t total_output = output_frames * channels;
    double *weights = (double *)calloc(output_frames, sizeof(double));
    size_t frame_size = 1024;
    size_t hop;
    size_t overlap;
    size_t search_radius;
    double stretch;

    if (!weights) {
        fprintf(stderr, "Out of memory stretching shifted layer.\n");
        return 0;
    }

    memset(output, 0, total_output * sizeof(double));

    if (input_frames < 64 || output_frames < 64) {
        double ratio = (double)input_frames / (double)output_frames;
        for (size_t frame = 0; frame < output_frames; frame++) {
            double pos = (double)frame * ratio;
            for (uint16_t ch = 0; ch < channels; ch++) {
                output[frame * channels + ch] =
                    sample_buffer_linear(input, input_frames, channels, pos, ch);
            }
        }
        free(weights);
        return 1;
    }

    if (frame_size > input_frames) frame_size = input_frames;
    if (frame_size > output_frames) frame_size = output_frames;
    hop = frame_size / 4;
    if (hop < 1) hop = 1;
    overlap = frame_size - hop;
    search_radius = hop;

    stretch = (double)output_frames / (double)input_frames;

    for (size_t out_pos = 0; out_pos < output_frames; out_pos += hop) {
        double nominal = (double)out_pos / stretch;
        size_t nominal_pos = (size_t)lrint(nominal);
        size_t in_pos;

        if (out_pos == 0) {
            in_pos = 0;
        } else {
            in_pos = find_best_overlap(input, output, weights, input_frames,
                                       output_frames, channels, nominal_pos,
                                       out_pos, frame_size, overlap,
                                       search_radius);
        }

        for (size_t i = 0; i < frame_size; i++) {
            size_t in_frame = in_pos + i;
            size_t out_frame = out_pos + i;
            double window;

            if (in_frame >= input_frames || out_frame >= output_frames) break;
            window = hann_window(i, frame_size);
            weights[out_frame] += window;

            for (uint16_t ch = 0; ch < channels; ch++) {
                output[out_frame * channels + ch] +=
                    input[in_frame * channels + ch] * window;
            }
        }
    }

    for (size_t frame = 0; frame < output_frames; frame++) {
        if (weights[frame] > 0.000001) {
            for (uint16_t ch = 0; ch < channels; ch++) {
                output[frame * channels + ch] /= weights[frame];
            }
        } else {
            double pos = (double)frame / stretch;
            for (uint16_t ch = 0; ch < channels; ch++) {
                output[frame * channels + ch] =
                    sample_buffer_linear(input, input_frames, channels, pos, ch);
            }
        }
    }

    free(weights);
    return 1;
}

static double source_sample_or_zero(const Wav *src, long frame, uint16_t channel) {
    if (frame < 0 || (size_t)frame >= src->frame_count) return 0.0;
    return (double)src->samples[(size_t)frame * src->channels + channel];
}

static double layer_sample_or_zero(const double *layer,
                                   size_t frame_count,
                                   uint16_t channels,
                                   long frame,
                                   uint16_t channel) {
    if (frame < 0 || (size_t)frame >= frame_count) return 0.0;
    return layer[(size_t)frame * channels + channel];
}

static void smooth_log_magnitudes(const double *mag, double *env, size_t bins, size_t radius) {
    for (size_t k = 0; k < bins; k++) {
        size_t start = k > radius ? k - radius : 0;
        size_t end = k + radius;
        double sum = 0.0;
        size_t count = 0;

        if (end >= bins) end = bins - 1;
        for (size_t i = start; i <= end; i++) {
            sum += log(mag[i] + 1.0);
            count++;
        }
        env[k] = count ? exp(sum / (double)count) : mag[k];
    }
}

static double envelope_at(const double *env, size_t bins, double bin_pos) {
    size_t bin0;
    size_t bin1;
    double frac;

    if (bin_pos <= 0.0) return env[0];
    if (bin_pos >= (double)(bins - 1)) return env[bins - 1];

    bin0 = (size_t)bin_pos;
    bin1 = bin0 + 1;
    frac = bin_pos - (double)bin0;
    return env[bin0] + (env[bin1] - env[bin0]) * frac;
}

static int apply_formant_warp_preserve(const Wav *src,
                                       double semitones,
                                       double strength,
                                       double *layer) {
    const double pi = 3.14159265358979323846;
    const size_t frame_size = 1024;
    const size_t hop = frame_size / 4;
    const size_t bins = frame_size / 2 + 1;
    const size_t smooth_radius = 42;
    size_t total_samples = src->frame_count * src->channels;
    double pitch_ratio = pow(2.0, semitones / 12.0);
    double *corrected = (double *)calloc(total_samples, sizeof(double));
    double *weights = (double *)calloc(src->frame_count, sizeof(double));
    double *src_mag = (double *)malloc(bins * sizeof(double));
    double *src_env = (double *)malloc(bins * sizeof(double));
    double *real = (double *)malloc(bins * sizeof(double));
    double *imag = (double *)malloc(bins * sizeof(double));

    if (!corrected || !weights || !src_mag || !src_env || !real || !imag) {
        fprintf(stderr, "Out of memory applying formant warp.\n");
        free(corrected);
        free(weights);
        free(src_mag);
        free(src_env);
        free(real);
        free(imag);
        return 0;
    }

    if (strength <= 0.0) strength = 1.0;

    for (size_t start = 0; start < src->frame_count; start += hop) {
        for (uint16_t ch = 0; ch < src->channels; ch++) {
            for (size_t k = 0; k < bins; k++) {
                double src_re = 0.0;
                double src_im = 0.0;
                double layer_re = 0.0;
                double layer_im = 0.0;

                for (size_t n = 0; n < frame_size; n++) {
                    long frame = (long)start + (long)n;
                    double window = hann_window(n, frame_size);
                    double angle = (2.0 * pi * (double)k * (double)n) / (double)frame_size;
                    double c = cos(angle);
                    double s = sin(angle);
                    double src_sample = source_sample_or_zero(src, frame, ch) * window;
                    double layer_sample =
                        layer_sample_or_zero(layer, src->frame_count, src->channels,
                                             frame, ch) * window;

                    src_re += src_sample * c;
                    src_im -= src_sample * s;
                    layer_re += layer_sample * c;
                    layer_im -= layer_sample * s;
                }

                src_mag[k] = sqrt(src_re * src_re + src_im * src_im);
                real[k] = layer_re;
                imag[k] = layer_im;
            }

            smooth_log_magnitudes(src_mag, src_env, bins, smooth_radius);

            for (size_t k = 0; k < bins; k++) {
                double current_formant_bin = (double)k / pitch_ratio;
                double desired = src_env[k] + 1.0;
                double shifted = envelope_at(src_env, bins, current_formant_bin) + 1.0;
                double gain = pow(desired / shifted, strength);

                if (gain < 0.04) gain = 0.04;
                if (gain > 18.0) gain = 18.0;

                real[k] *= gain;
                imag[k] *= gain;
            }

            for (size_t n = 0; n < frame_size; n++) {
                long frame = (long)start + (long)n;
                double value = real[0];
                double window;

                if (frame < 0 || (size_t)frame >= src->frame_count) continue;

                for (size_t k = 1; k < bins - 1; k++) {
                    double angle = (2.0 * pi * (double)k * (double)n) / (double)frame_size;
                    value += 2.0 * (real[k] * cos(angle) - imag[k] * sin(angle));
                }

                value += real[bins - 1] * cos(pi * (double)n);
                value /= (double)frame_size;

                window = hann_window(n, frame_size);
                corrected[(size_t)frame * src->channels + ch] += value * window;
                if (ch == 0) weights[(size_t)frame] += window;
            }
        }
    }

    for (size_t frame = 0; frame < src->frame_count; frame++) {
        if (weights[frame] > 0.000001) {
            for (uint16_t ch = 0; ch < src->channels; ch++) {
                layer[frame * src->channels + ch] =
                    corrected[frame * src->channels + ch] / weights[frame];
            }
        }
    }

    free(corrected);
    free(weights);
    free(src_mag);
    free(src_env);
    free(real);
    free(imag);
    return 1;
}

static int compute_lpc_for_frame(const Wav *src,
                                 size_t start,
                                 uint16_t channel,
                                 int order,
                                 size_t frame_size,
                                 double *a) {
    double *r = (double *)calloc((size_t)order + 1, sizeof(double));
    double *prev = (double *)calloc((size_t)order + 1, sizeof(double));
    double error;

    if (!r || !prev) {
        free(r);
        free(prev);
        return 0;
    }

    for (int lag = 0; lag <= order; lag++) {
        for (size_t n = (size_t)lag; n < frame_size; n++) {
            long frame_a = (long)start + (long)n;
            long frame_b = frame_a - lag;
            double wa = hann_window(n, frame_size);
            double wb = hann_window(n - (size_t)lag, frame_size);
            double xa = source_sample_or_zero(src, frame_a, channel) * wa;
            double xb = source_sample_or_zero(src, frame_b, channel) * wb;

            r[lag] += xa * xb;
        }
    }

    memset(a, 0, ((size_t)order + 1) * sizeof(double));
    a[0] = 1.0;
    error = r[0];

    if (error < 1.0) {
        free(r);
        free(prev);
        return 1;
    }

    for (int i = 1; i <= order; i++) {
        double acc = r[i];
        double k;

        for (int j = 1; j < i; j++) {
            acc += a[j] * r[i - j];
        }

        k = -acc / error;
        if (k > 0.98) k = 0.98;
        if (k < -0.98) k = -0.98;

        memcpy(prev, a, ((size_t)order + 1) * sizeof(double));
        a[i] = k;
        for (int j = 1; j < i; j++) {
            a[j] = prev[j] + k * prev[i - j];
        }

        error *= 1.0 - k * k;
        if (error < 1.0) break;
    }

    free(r);
    free(prev);
    return 1;
}

static int build_lpc_excitation(const Wav *src,
                                int order,
                                size_t frame_size,
                                size_t hop,
                                double *excitation) {
    size_t total_samples = src->frame_count * src->channels;
    double *weights = (double *)calloc(src->frame_count, sizeof(double));
    double *a = (double *)malloc(((size_t)order + 1) * sizeof(double));

    if (!weights || !a) {
        fprintf(stderr, "Out of memory building LPC excitation.\n");
        free(weights);
        free(a);
        return 0;
    }

    memset(excitation, 0, total_samples * sizeof(double));

    for (size_t start = 0; start < src->frame_count; start += hop) {
        for (uint16_t ch = 0; ch < src->channels; ch++) {
            if (!compute_lpc_for_frame(src, start, ch, order, frame_size, a)) {
                free(weights);
                free(a);
                return 0;
            }

            for (size_t n = 0; n < frame_size; n++) {
                long frame = (long)start + (long)n;
                double residual = source_sample_or_zero(src, frame, ch);
                double window;

                if ((size_t)frame >= src->frame_count) break;

                for (int i = 1; i <= order; i++) {
                    residual += a[i] * source_sample_or_zero(src, frame - i, ch);
                }

                window = hann_window(n, frame_size);
                excitation[(size_t)frame * src->channels + ch] += residual * window;
                if (ch == 0) weights[(size_t)frame] += window;
            }
        }
    }

    for (size_t frame = 0; frame < src->frame_count; frame++) {
        if (weights[frame] > 0.000001) {
            for (uint16_t ch = 0; ch < src->channels; ch++) {
                excitation[frame * src->channels + ch] /= weights[frame];
            }
        }
    }

    free(weights);
    free(a);
    return 1;
}

static int synthesize_lpc_layer(const Wav *src,
                                const double *excitation,
                                int order,
                                size_t frame_size,
                                size_t hop,
                                double *layer) {
    size_t total_samples = src->frame_count * src->channels;
    double *weights = (double *)calloc(src->frame_count, sizeof(double));
    double *a = (double *)malloc(((size_t)order + 1) * sizeof(double));
    double *hist = (double *)calloc((size_t)order, sizeof(double));

    if (!weights || !a || !hist) {
        fprintf(stderr, "Out of memory synthesizing LPC layer.\n");
        free(weights);
        free(a);
        free(hist);
        return 0;
    }

    memset(layer, 0, total_samples * sizeof(double));

    for (size_t start = 0; start < src->frame_count; start += hop) {
        for (uint16_t ch = 0; ch < src->channels; ch++) {
            memset(hist, 0, (size_t)order * sizeof(double));

            if (!compute_lpc_for_frame(src, start, ch, order, frame_size, a)) {
                free(weights);
                free(a);
                free(hist);
                return 0;
            }

            for (size_t n = 0; n < frame_size; n++) {
                size_t frame = start + n;
                double y;
                double window;

                if (frame >= src->frame_count) break;

                y = excitation[frame * src->channels + ch];
                for (int i = 1; i <= order; i++) {
                    y -= a[i] * hist[(size_t)i - 1];
                }

                for (int i = order - 1; i > 0; i--) {
                    hist[(size_t)i] = hist[(size_t)i - 1];
                }
                if (order > 0) hist[0] = y;

                window = hann_window(n, frame_size);
                layer[frame * src->channels + ch] += y * window;
                if (ch == 0) weights[frame] += window;
            }
        }
    }

    for (size_t frame = 0; frame < src->frame_count; frame++) {
        if (weights[frame] > 0.000001) {
            for (uint16_t ch = 0; ch < src->channels; ch++) {
                layer[frame * src->channels + ch] /= weights[frame];
            }
        }
    }

    free(weights);
    free(a);
    free(hist);
    return 1;
}

static int make_lpc_formant_layer(const Wav *src,
                                  double semitones,
                                  double formant_strength,
                                  double *layer) {
    const int order = 18;
    const size_t frame_size = 1024;
    const size_t hop = frame_size / 4;
    double ratio = pow(2.0, semitones / 12.0);
    size_t shifted_frames;
    size_t total_samples = src->frame_count * src->channels;
    double *excitation = (double *)malloc(total_samples * sizeof(double));
    double *shifted = NULL;
    double *shifted_excitation = (double *)malloc(total_samples * sizeof(double));

    if (!excitation || !shifted_excitation || ratio <= 0.0) {
        fprintf(stderr, "Out of memory creating LPC formant layer.\n");
        free(excitation);
        free(shifted_excitation);
        return 0;
    }

    if (!build_lpc_excitation(src, order, frame_size, hop, excitation)) {
        free(excitation);
        free(shifted_excitation);
        return 0;
    }

    shifted_frames = (size_t)ceil((double)src->frame_count / ratio);
    if (shifted_frames < 2) shifted_frames = 2;

    shifted = (double *)malloc(shifted_frames * src->channels * sizeof(double));
    if (!shifted) {
        fprintf(stderr, "Out of memory shifting LPC excitation.\n");
        free(excitation);
        free(shifted_excitation);
        return 0;
    }

    for (size_t frame = 0; frame < shifted_frames; frame++) {
        double src_pos = (double)frame * ratio;
        for (uint16_t ch = 0; ch < src->channels; ch++) {
            shifted[frame * src->channels + ch] =
                sample_buffer_linear(excitation, src->frame_count,
                                     src->channels, src_pos, ch);
        }
    }

    if (!stretch_wsola(shifted, shifted_frames, src->channels,
                       src->frame_count, shifted_excitation)) {
        free(excitation);
        free(shifted);
        free(shifted_excitation);
        return 0;
    }

    if (!synthesize_lpc_layer(src, shifted_excitation, order, frame_size, hop, layer)) {
        free(excitation);
        free(shifted);
        free(shifted_excitation);
        return 0;
    }

    if (!apply_formant_warp_preserve(src, semitones, formant_strength, layer)) {
        free(excitation);
        free(shifted);
        free(shifted_excitation);
        return 0;
    }

    free(excitation);
    free(shifted);
    free(shifted_excitation);
    return 1;
}

#ifndef USE_EMBEDDED_RUBBERBAND
static int copy_wav_to_layer_resized(const Wav *src, const Wav *processed, double *layer) {
    if (processed->audio_format != 1 ||
        processed->bits_per_sample != 16 ||
        processed->channels != src->channels) {
        fprintf(stderr, "Rubber Band output format did not match input format.\n");
        return 0;
    }

    if (processed->frame_count < 2) {
        fprintf(stderr, "Rubber Band output was too short.\n");
        return 0;
    }

    if (processed->frame_count == src->frame_count) {
        size_t total_samples = src->frame_count * src->channels;
        for (size_t i = 0; i < total_samples; i++) {
            layer[i] = (double)processed->samples[i];
        }
        return 1;
    }

    for (size_t frame = 0; frame < src->frame_count; frame++) {
        double pos = ((double)frame * (double)(processed->frame_count - 1)) /
                     (double)(src->frame_count - 1);
        for (uint16_t ch = 0; ch < src->channels; ch++) {
            layer[frame * src->channels + ch] =
                sample_wav_linear(processed, pos, ch);
        }
    }

    return 1;
}
#endif

#ifdef USE_EMBEDDED_RUBBERBAND
static void free_channel_buffers(float **buffers, uint16_t channels) {
    if (!buffers) return;
    for (uint16_t ch = 0; ch < channels; ch++) {
        free(buffers[ch]);
    }
    free(buffers);
}

static float **alloc_channel_buffers(uint16_t channels, size_t frames) {
    float **buffers = (float **)calloc(channels, sizeof(float *));

    if (!buffers) return NULL;
    for (uint16_t ch = 0; ch < channels; ch++) {
        buffers[ch] = (float *)calloc(frames, sizeof(float));
        if (!buffers[ch]) {
            free_channel_buffers(buffers, channels);
            return NULL;
        }
    }
    return buffers;
}

static int append_rubberband_output(double **out,
                                    size_t *frames,
                                    size_t *capacity,
                                    uint16_t channels,
                                    float **buffers,
                                    size_t count) {
    if (*frames + count > *capacity) {
        size_t new_capacity = *capacity ? *capacity : 4096;
        double *grown;

        while (new_capacity < *frames + count) {
            new_capacity *= 2;
        }

        grown = (double *)realloc(*out, new_capacity * channels * sizeof(double));
        if (!grown) return 0;

        *out = grown;
        *capacity = new_capacity;
    }

    for (size_t i = 0; i < count; i++) {
        for (uint16_t ch = 0; ch < channels; ch++) {
            (*out)[(*frames + i) * channels + ch] = (double)buffers[ch][i] * 32768.0;
        }
    }

    *frames += count;
    return 1;
}

static int make_embedded_rubberband_layer(const Wav *src,
                                          double semitones,
                                          int preserve_formants,
                                          const RubberBandArgs *rb_args,
                                          double *layer) {
    const size_t block_size = 1024;
    RubberBandOptions options = RubberBandOptionProcessOffline;
    double pitch_scale = pow(2.0, semitones / 12.0);
    RubberBandState state = NULL;
    float **input = NULL;
    float **output = NULL;
    double *processed = NULL;
    size_t processed_frames = 0;
    size_t processed_capacity = 0;
    int ok = 0;

    options |= rb_args->engine_finer ?
        RubberBandOptionEngineFiner : RubberBandOptionEngineFaster;

    if (rb_args->pitch_option == 1) options |= RubberBandOptionPitchHighQuality;
    if (rb_args->pitch_option == 2) options |= RubberBandOptionPitchHighConsistency;

    if (rb_args->window_option == 1) options |= RubberBandOptionWindowShort;
    if (rb_args->window_option == 2) options |= RubberBandOptionWindowLong;

    if (rb_args->transients_option == 1) options |= RubberBandOptionTransientsMixed;
    if (rb_args->transients_option == 2) options |= RubberBandOptionTransientsSmooth;

    if (rb_args->detector_option == 1) options |= RubberBandOptionDetectorPercussive;
    if (rb_args->detector_option == 2) options |= RubberBandOptionDetectorSoft;

    if (rb_args->phase_independent) options |= RubberBandOptionPhaseIndependent;
    if (rb_args->channels_together) options |= RubberBandOptionChannelsTogether;
    if (preserve_formants) options |= RubberBandOptionFormantPreserved;

    input = alloc_channel_buffers(src->channels, block_size);
    output = alloc_channel_buffers(src->channels, block_size);
    state = rubberband_new(src->sample_rate, src->channels, options, 1.0, pitch_scale);

    if (!input || !output || !state) {
        fprintf(stderr, "Out of memory starting embedded Rubber Band.\n");
        goto done;
    }

    rubberband_set_expected_input_duration(state, (unsigned int)src->frame_count);
    rubberband_set_max_process_size(state, (unsigned int)block_size);

    for (size_t pos = 0; pos < src->frame_count; pos += block_size) {
        size_t count = src->frame_count - pos;
        int final;

        if (count > block_size) count = block_size;
        final = (pos + count >= src->frame_count);

        for (uint16_t ch = 0; ch < src->channels; ch++) {
            for (size_t i = 0; i < count; i++) {
                input[ch][i] =
                    (float)((double)src->samples[(pos + i) * src->channels + ch] / 32768.0);
            }
        }

        rubberband_study(state, (const float *const *)input, (unsigned int)count, final);
    }

    for (size_t pos = 0; pos < src->frame_count; pos += block_size) {
        size_t count = src->frame_count - pos;
        int final;

        if (count > block_size) count = block_size;
        final = (pos + count >= src->frame_count);

        for (uint16_t ch = 0; ch < src->channels; ch++) {
            for (size_t i = 0; i < count; i++) {
                input[ch][i] =
                    (float)((double)src->samples[(pos + i) * src->channels + ch] / 32768.0);
            }
        }

        rubberband_process(state, (const float *const *)input, (unsigned int)count, final);

        for (;;) {
            int available = rubberband_available(state);
            unsigned int request;
            unsigned int got;

            if (available <= 0) break;
            request = (unsigned int)(available > (int)block_size ?
                                     (int)block_size : available);
            got = rubberband_retrieve(state, output, request);
            if (got == 0) break;

            if (!append_rubberband_output(&processed, &processed_frames,
                                          &processed_capacity, src->channels,
                                          output, got)) {
                fprintf(stderr, "Out of memory collecting Rubber Band output.\n");
                goto done;
            }
        }
    }

    for (;;) {
        int available = rubberband_available(state);
        unsigned int request;
        unsigned int got;

        if (available <= 0) break;
        request = (unsigned int)(available > (int)block_size ?
                                 (int)block_size : available);
        got = rubberband_retrieve(state, output, request);
        if (got == 0) break;

        if (!append_rubberband_output(&processed, &processed_frames,
                                      &processed_capacity, src->channels,
                                      output, got)) {
            fprintf(stderr, "Out of memory collecting Rubber Band output.\n");
            goto done;
        }
    }

    if (processed_frames < 2) {
        fprintf(stderr, "Embedded Rubber Band produced no audio.\n");
        goto done;
    }

    if (processed_frames == src->frame_count) {
        size_t total_samples = src->frame_count * src->channels;
        for (size_t i = 0; i < total_samples; i++) {
            layer[i] = processed[i];
        }
    } else {
        for (size_t frame = 0; frame < src->frame_count; frame++) {
            double pos = ((double)frame * (double)(processed_frames - 1)) /
                         (double)(src->frame_count - 1);
            for (uint16_t ch = 0; ch < src->channels; ch++) {
                layer[frame * src->channels + ch] =
                    sample_buffer_linear(processed, processed_frames,
                                         src->channels, pos, ch);
            }
        }
    }

    ok = 1;

done:
    if (state) rubberband_delete(state);
    free_channel_buffers(input, src->channels);
    free_channel_buffers(output, src->channels);
    free(processed);
    return ok;
}
#endif

#ifndef USE_EMBEDDED_RUBBERBAND
static int make_rubberband_layer(const char *input_path,
                                 const Wav *src,
                                 double semitones,
                                 int preserve_formants,
                                 const RubberBandArgs *rb_args,
                                 double *layer) {
    char temp_output[256];
    char quoted_rubberband[1024];
    char quoted_input[1024];
    char quoted_output[1024];
    char command[3400];
    const char *rubberband_exe = find_rubberband_executable();
    Wav processed;
    int exit_code;
    int ok;

    snprintf(temp_output, sizeof(temp_output),
             "multipitch_rubberband_%lu_%ld.wav",
             (unsigned long)time(NULL), (long)lrint(semitones * 1000.0));

    if (!command_exe_arg(rubberband_exe, quoted_rubberband, sizeof(quoted_rubberband)) ||
        !quote_arg(input_path, quoted_input, sizeof(quoted_input)) ||
        !quote_arg(temp_output, quoted_output, sizeof(quoted_output))) {
        fprintf(stderr, "Path is too long for Rubber Band command.\n");
        return 0;
    }

    snprintf(command, sizeof(command),
             "%s %s %s -p %.12g %s %s",
             quoted_rubberband,
             rb_args->cli_args,
             preserve_formants ? "-F" : "",
             semitones,
             quoted_input,
             quoted_output);

    exit_code = system(command);
    if (exit_code != 0) {
        snprintf(command, sizeof(command),
                 "%s %s %s -p %.12g %s %s",
                 quoted_rubberband,
                 rb_args->cli_args,
                 preserve_formants ? "-F" : "",
                 semitones,
                 quoted_input,
                 quoted_output);
        exit_code = system(command);
    }

    if (exit_code != 0) {
        fprintf(stderr,
                "Rubber Band failed. Put rubberband.exe in .\\import\\, set RUBBERBAND_EXE, or install it on PATH.\n");
        remove(temp_output);
        return 0;
    }

    memset(&processed, 0, sizeof(processed));
    if (!read_wav(temp_output, &processed)) {
        remove(temp_output);
        return 0;
    }

    ok = copy_wav_to_layer_resized(src, &processed, layer);
    free(processed.samples);
    remove(temp_output);
    return ok;
}
#endif

static int make_pitch_layer(const Wav *src,
                            const char *input_path,
                            double semitones,
                            int preserve_formants,
                            int backend,
                            double formant_strength,
                            const RubberBandArgs *rb_args,
                            double *layer) {
    double ratio = pow(2.0, semitones / 12.0);
    size_t shifted_frames;
    double *shifted;

    if (ratio <= 0.0) {
        fprintf(stderr, "Invalid pitch ratio.\n");
        return 0;
    }

    if (backend == BACKEND_SIGNALSMITH && fabs(semitones) > 0.000001) {
#ifdef USE_EMBEDDED_SIGNALSMITH
        (void)input_path;
        (void)formant_strength;
        (void)rb_args;
        return signalsmith_make_pitch_layer(src->samples, (unsigned long)src->frame_count,
                                            src->channels, src->sample_rate,
                                            semitones, preserve_formants, layer);
#else
        fprintf(stderr, "Signalsmith backend was not compiled in. Rebuild with USE_EMBEDDED_SIGNALSMITH.\n");
        return 0;
#endif
    }

    if (backend == BACKEND_BUNGEE && fabs(semitones) > 0.000001) {
#ifdef USE_EMBEDDED_BUNGEE
        (void)input_path;
        (void)formant_strength;
        (void)rb_args;
        return bungee_make_pitch_layer(src->samples, (unsigned long)src->frame_count,
                                       src->channels, src->sample_rate,
                                       semitones, preserve_formants, layer);
#else
        fprintf(stderr, "Bungee backend was not compiled in. Rebuild with USE_EMBEDDED_BUNGEE.\n");
        return 0;
#endif
    }

    if (backend == BACKEND_SOUNDTOUCH && fabs(semitones) > 0.000001) {
#ifdef USE_EMBEDDED_SOUNDTOUCH
        (void)input_path;
        (void)formant_strength;
        (void)rb_args;
        return soundtouch_make_pitch_layer(src->samples, (unsigned long)src->frame_count,
                                           src->channels, src->sample_rate,
                                           semitones, preserve_formants, layer);
#else
        fprintf(stderr, "SoundTouch backend was not compiled in. Rebuild with USE_EMBEDDED_SOUNDTOUCH.\n");
        return 0;
#endif
    }

    if (backend == BACKEND_RUBBERBAND && fabs(semitones) > 0.000001) {
#ifdef USE_EMBEDDED_RUBBERBAND
        (void)input_path;
        return make_embedded_rubberband_layer(src, semitones,
                                              preserve_formants,
                                              rb_args, layer);
#else
        return make_rubberband_layer(input_path, src, semitones,
                                     preserve_formants, rb_args, layer);
#endif
    }

    if (preserve_formants && fabs(semitones) > 0.000001) {
        return make_lpc_formant_layer(src, semitones, formant_strength, layer);
    }

    if (fabs(semitones) < 0.000001) {
        size_t total_samples = src->frame_count * src->channels;
        for (size_t i = 0; i < total_samples; i++) {
            layer[i] = (double)src->samples[i];
        }
        return 1;
    }

    shifted_frames = (size_t)ceil((double)src->frame_count / ratio);
    if (shifted_frames < 2) shifted_frames = 2;

    shifted = (double *)malloc(shifted_frames * src->channels * sizeof(double));
    if (!shifted) {
        fprintf(stderr, "Out of memory creating shifted layer.\n");
        return 0;
    }

    for (size_t frame = 0; frame < shifted_frames; frame++) {
        double src_pos = (double)frame * ratio;
        for (uint16_t ch = 0; ch < src->channels; ch++) {
            shifted[frame * src->channels + ch] =
                sample_wav_linear(src, src_pos, ch);
        }
    }

    if (!stretch_wsola(shifted, shifted_frames, src->channels, src->frame_count, layer)) {
        free(shifted);
        return 0;
    }

    free(shifted);
    return 1;
}

static int mix_pitches(const Wav *src,
                       const char *input_path,
                       const double *pitches,
                       size_t pitch_count,
                       int preserve_formants,
                       int backend,
                       double formant_strength,
                       const RubberBandArgs *rb_args,
                       int normalize_output,
                       int16_t **out_samples) {
    size_t total_samples = src->frame_count * src->channels;
    double *mix = (double *)calloc(total_samples, sizeof(double));
    double *layer = (double *)malloc(total_samples * sizeof(double));
    int16_t *result = (int16_t *)malloc(total_samples * sizeof(int16_t));
    double peak = 1.0;

    if (!mix || !layer || !result) {
        fprintf(stderr, "Out of memory mixing audio.\n");
        free(mix);
        free(layer);
        free(result);
        return 0;
    }

    for (size_t p = 0; p < pitch_count; p++) {
        if (!make_pitch_layer(src, input_path, pitches[p], preserve_formants,
                              backend, formant_strength,
                              rb_args, layer)) {
            free(mix);
            free(layer);
            free(result);
            return 0;
        }

        for (size_t i = 0; i < total_samples; i++) {
            mix[i] += layer[i];
        }
    }

    for (size_t i = 0; i < total_samples; i++) {
        double abs_value = fabs(mix[i]);
        if (abs_value > peak) peak = abs_value;
    }

    if (normalize_output && peak > 32767.0) {
        double scale = 32767.0 / peak;
        for (size_t i = 0; i < total_samples; i++) {
            result[i] = clamp_i16(mix[i] * scale);
        }
    } else {
        for (size_t i = 0; i < total_samples; i++) {
            result[i] = clamp_i16(mix[i]);
        }
    }

    free(mix);
    free(layer);
    *out_samples = result;
    return 1;
}

static void print_usage(const char *program) {
    fprintf(stderr,
            "Usage:\n"
            "  %s input.wav output.wav semitones[,semitones...] [--preserve-formants] [--basic] [--rubberband-args \"args\"]\n\n"
            "Example:\n"
            "  %s resize.wav whar.wav -12,-7,0,4,7,12\n\n"
            "  %s resize.wav whar_up.wav 4 --preserve-formants\n\n"
            "Notes:\n"
            "  Input must be uncompressed 16-bit PCM WAV.\n"
            "  Rubber Band is the default pitch engine.\n"
            "  Embedded builds use the Rubber Band library directly.\n"
            "  Non-embedded builds use the Rubber Band CLI.\n"
            "  --backend rubberband|signalsmith|bungee|soundtouch|basic selects the pitch engine.\n"
            "  --basic uses the old built-in resample/WSOLA fallback.\n"
            "  --bungee uses the embedded open-source Bungee Basic engine.\n"
            "  --soundtouch uses the embedded SoundTouch engine.\n"
            "  --no-normalize disables final peak normalization and clamps instead.\n"
            "  --rubberband-args accepts supported Rubber Band-style flags, e.g. \"-2 --pitch-hc --window-short\".\n"
            "This Program is made by Beebo\n",
            program, program, program);
}

int main(int argc, char **argv) {
    Wav input;
    double *pitches = NULL;
    int16_t *mixed = NULL;
    size_t pitch_count = 0;
    int preserve_formants = 0;
    int backend = BACKEND_RUBBERBAND;
    int normalize_output = 1;
    double formant_strength = 1.6;
    RubberBandArgs rb_args;
    int ok = 0;

    init_rubberband_args(&rb_args);

    if (argc < 4) {
        print_usage(argv[0]);
        return 1;
    }

    for (int i = 4; i < argc; i++) {
        if (strcmp(argv[i], "--preserve-formants") == 0 ||
            strcmp(argv[i], "--formants") == 0) {
            preserve_formants = 1;
            if (backend == BACKEND_BASIC) backend = BACKEND_RUBBERBAND;
        } else if (strcmp(argv[i], "--rubberband") == 0) {
            backend = BACKEND_RUBBERBAND;
        } else if (strcmp(argv[i], "--signalsmith") == 0) {
            backend = BACKEND_SIGNALSMITH;
        } else if (strcmp(argv[i], "--bungee") == 0) {
            backend = BACKEND_BUNGEE;
        } else if (strcmp(argv[i], "--soundtouch") == 0) {
            backend = BACKEND_SOUNDTOUCH;
        } else if (strcmp(argv[i], "--backend") == 0) {
            if (i + 1 >= argc) {
                fprintf(stderr, "--backend needs rubberband, signalsmith, bungee, soundtouch, or basic.\n");
                print_usage(argv[0]);
                return 1;
            }
            i++;
            if (strcmp(argv[i], "rubberband") == 0) {
                backend = BACKEND_RUBBERBAND;
            } else if (strcmp(argv[i], "signalsmith") == 0) {
                backend = BACKEND_SIGNALSMITH;
            } else if (strcmp(argv[i], "bungee") == 0) {
                backend = BACKEND_BUNGEE;
            } else if (strcmp(argv[i], "soundtouch") == 0) {
                backend = BACKEND_SOUNDTOUCH;
            } else if (strcmp(argv[i], "basic") == 0) {
                backend = BACKEND_BASIC;
            } else {
                fprintf(stderr, "Unknown backend: %s\n", argv[i]);
                return 1;
            }
        } else if (strcmp(argv[i], "--basic") == 0 ||
                   strcmp(argv[i], "--no-rubberband") == 0) {
            backend = BACKEND_BASIC;
        } else if (strcmp(argv[i], "--no-normalize") == 0 ||
                   strcmp(argv[i], "--no-normalise") == 0) {
            normalize_output = 0;
        } else if (strcmp(argv[i], "--normalize") == 0 ||
                   strcmp(argv[i], "--normalise") == 0) {
            normalize_output = 1;
        } else if (strcmp(argv[i], "--rubberband-args") == 0 ||
                   strcmp(argv[i], "--rb-args") == 0) {
            if (i + 1 >= argc) {
                fprintf(stderr, "--rubberband-args needs a quoted argument string.\n");
                print_usage(argv[0]);
                return 1;
            }
            backend = BACKEND_RUBBERBAND;
            if (!parse_rubberband_args(argv[++i], &rb_args, &preserve_formants)) {
                return 1;
            }
        } else if (strcmp(argv[i], "--formant-strength") == 0) {
            char *end = NULL;
            if (i + 1 >= argc) {
                fprintf(stderr, "--formant-strength needs a number.\n");
                print_usage(argv[0]);
                return 1;
            }
            formant_strength = strtod(argv[++i], &end);
            if (!end || *end != '\0' || formant_strength < 0.0) {
                fprintf(stderr, "Invalid formant strength.\n");
                return 1;
            }
            preserve_formants = 1;
        } else {
            fprintf(stderr, "Unknown option: %s\n", argv[i]);
            print_usage(argv[0]);
            return 1;
        }
    }

    if (!parse_pitch_list(argv[3], &pitches, &pitch_count)) return 1;
    if (backend == BACKEND_BUNGEE && preserve_formants) {
        fprintf(stderr,
                "Note: embedded Bungee Basic does not expose a formant-preserve switch; "
                "--preserve-formants is ignored for this backend.\n");
    }
    if (backend == BACKEND_SOUNDTOUCH && preserve_formants) {
        fprintf(stderr,
                "Note: embedded SoundTouch does not expose a formant-preserve switch; "
                "--preserve-formants is ignored for this backend.\n");
    }
    if (!read_wav(argv[1], &input)) {
        free(pitches);
        return 1;
    }

    if (input.frame_count < 2) {
        fprintf(stderr, "Input WAV is too short.\n");
    } else if (mix_pitches(&input, argv[1], pitches, pitch_count,
                           preserve_formants, backend,
                           formant_strength, &rb_args,
                           normalize_output, &mixed)) {
        ok = write_wav(argv[2], &input, mixed);
    }

    free(input.samples);
    free(pitches);
    free(mixed);
    return ok ? 0 : 1;
}
