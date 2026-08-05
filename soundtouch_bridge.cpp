#include "soundtouch_bridge.h"

#include <SoundTouch.h>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <vector>

static double read_processed_sample(const std::vector<soundtouch::SAMPLETYPE> &samples,
                                    unsigned long frames,
                                    unsigned int channels,
                                    double pos,
                                    unsigned int channel) {
    if (frames == 0) return 0.0;
    if (frames == 1) return samples[channel];

    if (pos <= 0.0) return samples[channel];
    double max_pos = static_cast<double>(frames - 1);
    if (pos >= max_pos) return samples[(frames - 1) * channels + channel];

    unsigned long i0 = static_cast<unsigned long>(pos);
    unsigned long i1 = i0 + 1;
    double frac = pos - static_cast<double>(i0);
    double a = samples[i0 * channels + channel];
    double b = samples[i1 * channels + channel];
    return a + (b - a) * frac;
}

static void drain_soundtouch(soundtouch::SoundTouch &st,
                             unsigned int channels,
                             std::vector<soundtouch::SAMPLETYPE> &processed) {
    const unsigned int drain_frames = 4096;
    std::vector<soundtouch::SAMPLETYPE> block(drain_frames * channels);

    for (;;) {
        unsigned int got = st.receiveSamples(block.data(), drain_frames);
        if (got == 0) break;
        processed.insert(processed.end(), block.begin(), block.begin() + got * channels);
    }
}

int soundtouch_make_pitch_layer(const short *input,
                                unsigned long frame_count,
                                unsigned int channels,
                                unsigned int sample_rate,
                                double semitones,
                                int preserve_formants,
                                double *output) {
    (void)preserve_formants;

    if (!input || !output || frame_count == 0 || channels == 0 || sample_rate == 0) {
        return 0;
    }

    try {
        soundtouch::SoundTouch st;
        st.setSampleRate(sample_rate);
        st.setChannels(channels);
        st.setPitchSemiTones(semitones);
        st.setTempo(1.0);
        st.setRate(1.0);
        st.setSetting(SETTING_USE_AA_FILTER, 1);
        st.setSetting(SETTING_AA_FILTER_LENGTH, 64);
        st.setSetting(SETTING_USE_QUICKSEEK, 0);

        const unsigned long chunk_frames = 4096;
        std::vector<soundtouch::SAMPLETYPE> chunk(chunk_frames * channels);
        std::vector<soundtouch::SAMPLETYPE> processed;
        processed.reserve(static_cast<size_t>(frame_count * channels));

        unsigned long offset = 0;
        while (offset < frame_count) {
            unsigned long count = std::min(chunk_frames, frame_count - offset);
            for (unsigned long i = 0; i < count; ++i) {
                for (unsigned int ch = 0; ch < channels; ++ch) {
                    size_t idx = static_cast<size_t>((offset + i) * channels + ch);
                    chunk[i * channels + ch] =
                        static_cast<soundtouch::SAMPLETYPE>(input[idx] / 32768.0);
                }
            }

            st.putSamples(chunk.data(), static_cast<unsigned int>(count));
            drain_soundtouch(st, channels, processed);
            offset += count;
        }

        st.flush();
        drain_soundtouch(st, channels, processed);

        unsigned long processed_frames =
            static_cast<unsigned long>(processed.size() / channels);
        if (processed_frames == 0) {
            std::fill(output, output + static_cast<size_t>(frame_count * channels), 0.0);
            return 1;
        }

        if (processed_frames == frame_count) {
            for (unsigned long i = 0; i < frame_count; ++i) {
                for (unsigned int ch = 0; ch < channels; ++ch) {
                    output[i * channels + ch] =
                        static_cast<double>(processed[i * channels + ch]) * 32768.0;
                }
            }
            return 1;
        }

        double scale = frame_count > 1
            ? static_cast<double>(processed_frames - 1) / static_cast<double>(frame_count - 1)
            : 0.0;
        for (unsigned long i = 0; i < frame_count; ++i) {
            double pos = static_cast<double>(i) * scale;
            for (unsigned int ch = 0; ch < channels; ++ch) {
                output[i * channels + ch] =
                    read_processed_sample(processed, processed_frames, channels, pos, ch) * 32768.0;
            }
        }

        return 1;
    } catch (...) {
        return 0;
    }
}
