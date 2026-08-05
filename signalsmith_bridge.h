#ifndef SIGNALSMITH_BRIDGE_H
#define SIGNALSMITH_BRIDGE_H

#ifdef __cplusplus
extern "C" {
#endif

int signalsmith_make_pitch_layer(const short *input,
                                 unsigned long frame_count,
                                 unsigned int channels,
                                 unsigned int sample_rate,
                                 double semitones,
                                 int preserve_formants,
                                 double *output);

#ifdef __cplusplus
}
#endif

#endif
