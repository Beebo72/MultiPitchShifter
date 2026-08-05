#include "bungee_bridge.h"

#include <algorithm>
#include <cmath>
#include <climits>
#include <vector>

#include <bungee/Bungee.h>
#include <bungee/Stream.h>

extern "C" int bungee_make_pitch_layer(const short *input,
                                       unsigned long frame_count,
                                       unsigned int channels,
                                       unsigned int sample_rate,
                                       double semitones,
                                       int preserve_formants,
                                       double *output) {
    (void)preserve_formants;

    if (!input || !output || frame_count < 2 || channels == 0 ||
        sample_rate == 0 || frame_count > INT_MAX) {
        return 0;
    }

    try {
        const int channel_count = static_cast<int>(channels);
        const int frame_count_i = static_cast<int>(frame_count);
        const double pitch_ratio = std::pow(2.0, semitones / 12.0);
        const Bungee::SampleRates sample_rates{
            static_cast<int>(sample_rate),
            static_cast<int>(sample_rate)
        };

        const int max_input_frames = 2048;

        Bungee::Stretcher<Bungee::Basic> stretcher(sample_rates, channel_count, 0);
        Bungee::Stream<Bungee::Basic> stream(stretcher, max_input_frames, channel_count);

        std::vector<std::vector<float> > input_channels(channel_count);
        std::vector<std::vector<float> > output_channels(channel_count);
        std::vector<const float *> input_ptrs(channel_count);
        std::vector<float *> output_ptrs(channel_count);

        for (int ch = 0; ch < channel_count; ch++) {
            input_channels[ch].resize(frame_count);
            output_channels[ch].assign(max_input_frames, 0.0f);
            output_ptrs[ch] = output_channels[ch].data();
        }

        for (unsigned long frame = 0; frame < frame_count; frame++) {
            for (int ch = 0; ch < channel_count; ch++) {
                input_channels[ch][frame] =
                    static_cast<float>(input[frame * channels + ch] / 32768.0);
            }
        }

        std::fill(output, output + frame_count * channels, 0.0);

        unsigned long input_position = 0;
        unsigned long output_position = 0;
        int silent_flush_chunks = 0;

        while (output_position < frame_count) {
            const int remaining = frame_count_i - static_cast<int>(std::min<unsigned long>(input_position, frame_count));
            int count = remaining > 0 ? std::min(max_input_frames, remaining) : max_input_frames;
            const bool have_input = input_position < frame_count;

            for (int ch = 0; ch < channel_count; ch++) {
                std::fill(output_channels[ch].begin(), output_channels[ch].end(), 0.0f);
                input_ptrs[ch] = have_input ? input_channels[ch].data() + input_position : nullptr;
            }

            int got = stream.process(have_input ? input_ptrs.data() : nullptr,
                                     output_ptrs.data(),
                                     count, static_cast<double>(count),
                                     pitch_ratio);
            if (got <= 0) return 0;

            if (have_input) {
                input_position += static_cast<unsigned long>(count);
            } else if (++silent_flush_chunks > 32) {
                break;
            }

            const double position_end = stream.outputPosition();
            const double position_begin = position_end - static_cast<double>(got);
            int skip = 0;

            if (!std::isnan(position_begin) && position_begin != position_end) {
                double preroll_input = std::max(0.0, std::round(-position_begin));
                skip = static_cast<int>(std::round(
                    preroll_input * (static_cast<double>(got) /
                                     std::fabs(position_end - position_begin))));
                if (skip < 0) skip = 0;
                if (skip > got) skip = got;
            }

            for (int frame = skip; frame < got && output_position < frame_count; frame++) {
                for (int ch = 0; ch < channel_count; ch++) {
                    output[output_position * channels + ch] =
                        static_cast<double>(output_channels[ch][frame]) * 32768.0;
                }
                output_position++;
            }
        }

        return output_position > 0;
    } catch (...) {
        return 0;
    }
}
