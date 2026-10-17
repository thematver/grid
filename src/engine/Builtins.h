// Built-in, procedurally generated audio resources (no sample files ship with the plug-in).
// Implemented in modules/Builtins.cpp. All functions are message-thread only (they allocate).
#pragma once

#include "Module.h"
#include <memory>
#include <string>
#include <vector>

namespace weft
{
/** Names of the built-in samples for GRAIN, e.g. "rain_tin.wav". */
const std::vector<std::string>& builtinSampleNames();

/** Generates (and caches) a built-in sample by name; null if unknown. Mono or stereo, 48 kHz. */
std::shared_ptr<const AudioResource> builtinSample (const std::string& name);

/** Built-in wavetable for WAVE's TABLE mode index (0..5: Glass Bell, Vowels, Harmonic, Digital, Metal, Breath). */
std::shared_ptr<const AudioResource> builtinWavetable (int index);

/** Converts any sample into a wavetable (cycles resampled to kWaveFrameSize, up to 64 frames). */
std::shared_ptr<const AudioResource> makeWavetableFromSample (const AudioResource& sample, const std::string& name);

/** Wraps decoded audio as a sample resource (downmixes to at most 2 channels, caps length at 30 s). */
std::shared_ptr<const AudioResource> makeSampleResource (const std::string& name, float sampleRate,
                                                         int channels, std::vector<float> interleaved);
} // namespace weft
