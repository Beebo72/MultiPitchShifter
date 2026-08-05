#include "signalsmith_bridge.h"

#include <cstring>
#include "signalsmith-stretch.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <vector>

extern "C" int signalsmith_make_pitch_layer(const short *input,
                                            unsigned long frame_count,
                                            unsigned int channels,
                                            unsigned int sample_rate,
                                            double semitones,
                                            int preserve_formants,
                                            double *output) {
    using Stretch = signalsmith::stretch::SignalsmithStretch<float>;

    if (!input || !output || frame_count < 2 || channels == 0 || sample_rate == 0) {
        return 0;
    }

    try {
        std::vector<std::vector<float> > in(channels);
        std::vector<std::vector<float> > out(channels);
        std::vector<float *> in_ptrs(channels);
        std::vector<float *> out_ptrs(channels);
        Stretch stretch;

        for (unsigned int ch = 0; ch < channels; ch++) {
            in[ch].resize(frame_count);
            out[ch].assign(frame_count, 0.0f);
            in_ptrs[ch] = in[ch].data();
            out_ptrs[ch] = out[ch].data();
        }

        for (unsigned long frame = 0; frame < frame_count; frame++) {
            for (unsigned int ch = 0; ch < channels; ch++) {
                in[ch][frame] =
                    static_cast<float>(input[frame * channels + ch] / 32768.0);
            }
        }

        stretch.presetDefault(static_cast<int>(channels),
                              static_cast<float>(sample_rate));
        stretch.setTransposeSemitones(static_cast<float>(semitones),
                                      static_cast<float>(8000.0 / sample_rate));
        if (preserve_formants) {
            stretch.setFormantSemitones(0.0f, true);
            stretch.setFormantBase(static_cast<float>(100.0 / sample_rate));
        }

        if (!stretch.exact(in_ptrs.data(), static_cast<int>(frame_count),
                           out_ptrs.data(), static_cast<int>(frame_count))) {
            return 0;
        }

        for (unsigned long frame = 0; frame < frame_count; frame++) {
            for (unsigned int ch = 0; ch < channels; ch++) {
                output[frame * channels + ch] =
                    static_cast<double>(out[ch][frame]) * 32768.0;
            }
        }

        return 1;
    } catch (...) {
        return 0;
    }
}
