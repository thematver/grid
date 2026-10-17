// Normalized-value -> physical-value mappings. Shared by DSP code and display formatting so the
// numbers on screen are exactly what the audio engine does.
#pragma once

#include "DspUtil.h"
#include <cmath>

namespace weft::maps
{
using dsp::clamp01;

// Generic
inline float bipolar (float v) noexcept { return clamp01 (v) * 2.0f - 1.0f; }          // -1..1
inline float semis24 (float v) noexcept { return (clamp01 (v) - 0.5f) * 48.0f; }        // -24..+24 st
inline float semis12 (float v) noexcept { return (clamp01 (v) - 0.5f) * 24.0f; }        // -12..+12 st

// Sources
inline float oscDetuneCents (float v) noexcept { return clamp01 (v) * 50.0f; }
inline float noiseLevelGain (float v) noexcept { return std::pow (clamp01 (v), 1.75f); }  // 0.4 -> -14 dB
inline float grainSizeMs (float v) noexcept   { return 20.0f * std::pow (100.0f, clamp01 (v)); }   // 20 ms .. 2 s
inline float grainDensity (float v) noexcept  { return 1.0f * std::pow (200.0f, clamp01 (v)); }    // 1 .. 200 grains/s
inline float inputGainDb (float v) noexcept   { return (clamp01 (v) - 0.5f) * 48.0f; }            // -24..+24 dB
inline float inputGateDb (float v) noexcept   { return -80.0f + 80.0f * clamp01 (v); }            // v == 0 -> off
inline float pluckDecaySec (float v) noexcept { return 0.05f * std::pow (400.0f, clamp01 (v)); }   // 50 ms .. 20 s

// Shape
inline float filterCutoffHz (float v) noexcept { return 20.0f * std::pow (1000.0f, clamp01 (v)); } // 20 Hz .. 20 kHz
/** Resonance -> Q per stage. 0.38 -> 1.84 ("Q 1.8"). Steep tail towards self-oscillation. */
inline float filterQ (float v) noexcept
{
    v = clamp01 (v);
    float q = 0.7f + 3.0f * v;
    if (v > 0.8f) { const float t = (v - 0.8f) / 0.2f; q += t * t * t * 36.0f; }
    return q;
}
inline float foldGain (float v) noexcept { return 1.0f + 3.0f * std::pow (clamp01 (v), 1.3f); }   // 1x .. 4x
inline float driveGainDb (float v) noexcept { return -6.0f + 30.0f * clamp01 (v); }              // -6 .. +24 dB
inline float crushBits (float v) noexcept { return 16.0f - 14.0f * clamp01 (v); }                // 16 .. 2 bits
inline float ringFreqHz (float v) noexcept { return 0.5f * std::pow (10000.0f, clamp01 (v)); }  // 0.5 Hz .. 5 kHz
inline float ringRatio (float v) noexcept { return 0.25f * std::pow (32.0f, clamp01 (v)); }      // x0.25 .. x8
inline int   combNote (float v) noexcept { return 36 + (int) std::lround (clamp01 (v) * 48.0f); } // C1 .. C5, 0.5 = C3 (60)
inline float combFreeHz (float v) noexcept { return 20.0f * std::pow (250.0f, clamp01 (v)); }   // 20 Hz .. 5 kHz
inline float combFeedback (float v) noexcept { return 0.99f * clamp01 (v); }

// Space
inline float delayFreeMs (float v) noexcept { return std::pow (2000.0f, clamp01 (v)); }          // 1 ms .. 2 s
inline float delayFeedback (float v) noexcept { return 1.02f * clamp01 (v); }                    // up to ∞ (soft-limited)
inline float verbDecaySec (float v) noexcept { return 0.36f * std::pow (55.6f, clamp01 (v)); }   // 0.36 .. 20 s; 0.8 -> 9.0 s
inline bool  verbInfinite (float v) noexcept { return v >= 0.995f; }
inline float verbPreMs (float v) noexcept { return 250.0f * clamp01 (v) * clamp01 (v); }
inline float freezeFadeSec (float v) noexcept { return 0.02f * std::pow (3600.0f, clamp01 (v)); } // 20 ms .. 72 s; 0.5 -> 1.2 s
inline float freezeSizeMs (float v) noexcept { return 50.0f * std::pow (40.0f, clamp01 (v)); }   // 50 ms .. 2 s
inline float chorusRateHz (float v) noexcept { return 0.05f * std::pow (200.0f, clamp01 (v)); }  // 0.05 .. 10 Hz
inline float loopSpeed (float v) noexcept { return (clamp01 (v) - 0.5f) * 4.0f; }                // -2 .. +2
inline float loopLengthSec (float v) noexcept { return 0.1f * std::pow (80.0f, clamp01 (v)); }   // 0.1 .. 8 s

// Control
inline float lfoRateHz (float v) noexcept { return 0.01f * std::pow (5000.0f, clamp01 (v)); }    // 0.01 .. 50 Hz
inline float envAttackSec (float v) noexcept { return 0.0005f * std::pow (20000.0f, clamp01 (v)); } // 0.5 ms .. 10 s
inline float envTimeSec (float v) noexcept { return 0.001f * std::pow (20000.0f, clamp01 (v)); }    // 1 ms .. 20 s
inline float randRateHz (float v) noexcept { return 0.05f * std::pow (1000.0f, clamp01 (v)); }    // 0.05 .. 50 Hz
inline int   seqSteps (float v) noexcept { return 1 + (int) std::lround (clamp01 (v) * 7.0f); }
inline float followAttackSec (float v) noexcept { return 0.0001f * std::pow (10000.0f, clamp01 (v)); }
inline float followReleaseSec (float v) noexcept { return 0.001f * std::pow (5000.0f, clamp01 (v)); }
inline float followGainDb (float v) noexcept { return -12.0f + 36.0f * clamp01 (v); }
inline float driftRateHz (float v, int speedMode) noexcept
{
    const float base = speedMode == 0 ? 0.002f : (speedMode == 1 ? 0.02f : 0.2f);
    return base * std::pow (100.0f, clamp01 (v));   // slow: 0.002..0.2 Hz, med: 0.02..2 Hz, fast: 0.2..20 Hz
}
inline int   clockSteps (float v) noexcept { return 1 + (int) std::lround (clamp01 (v) * 15.0f); }
inline int   clockFill (float v, int steps) noexcept { return (int) std::lround (clamp01 (v) * (float) steps); }

/** LFO / RAND / SEQ / CLOCK sync table (beats). */
struct SyncRate { const char* name; float beats; };
inline constexpr SyncRate kSyncRates[] = {
    { "1/64", 0.0625f }, { "1/32", 0.125f }, { "1/16T", 1.0f / 6.0f }, { "1/16", 0.25f }, { "1/8T", 1.0f / 3.0f },
    { "1/16D", 0.375f }, { "1/8", 0.5f }, { "1/4T", 2.0f / 3.0f }, { "1/8D", 0.75f }, { "1/4", 1.0f },
    { "1/2T", 4.0f / 3.0f }, { "1/4D", 1.5f }, { "1/2", 2.0f }, { "1/1T", 8.0f / 3.0f }, { "1/2D", 3.0f },
    { "1 bar", 4.0f }, { "2 bars", 8.0f }, { "4 bars", 16.0f }, { "8 bars", 32.0f }, { "16 bars", 64.0f },
};
inline constexpr int kNumSyncRates = (int) (sizeof (kSyncRates) / sizeof (kSyncRates[0]));
inline int syncRateIndex (float v) noexcept { return (int) std::lround (clamp01 (v) * (float) (kNumSyncRates - 1)); }
inline float syncRateBeats (float v) noexcept { return kSyncRates[syncRateIndex (v)].beats; }

} // namespace weft::maps
