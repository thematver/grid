// Small, allocation-free DSP helpers shared by all modules.
#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <vector>

#if defined(__aarch64__) || defined(_M_ARM64)
 #define WEFT_ARM64 1
#elif defined(__x86_64__) || defined(_M_X64) || defined(__i386__)
 #define WEFT_X86 1
 #include <immintrin.h>
#endif

namespace weft::dsp
{
constexpr float kPi    = 3.14159265358979323846f;
constexpr float kTwoPi = 6.28318530717958647692f;

inline float clamp01 (float x) noexcept { return x < 0.0f ? 0.0f : (x > 1.0f ? 1.0f : x); }
inline float clampf (float x, float lo, float hi) noexcept { return x < lo ? lo : (x > hi ? hi : x); }
inline float lerp (float a, float b, float t) noexcept { return a + (b - a) * t; }

inline float dbToGain (float db) noexcept { return std::pow (10.0f, db * 0.05f); }
inline float gainToDb (float g) noexcept  { return g > 1.0e-9f ? 20.0f * std::log10 (g) : -180.0f; }

/** MIDI note (fractional) to Hz, A4 = 69 = 440 Hz. */
inline float noteToHz (float note) noexcept { return 440.0f * std::exp2 ((note - 69.0f) * (1.0f / 12.0f)); }

/** Exponential map of a normalized value: lo * (hi/lo)^v. */
inline float expMap (float v, float lo, float hi) noexcept { return lo * std::pow (hi / lo, v); }

/** Fast tanh-like soft clipper (Padé 3/2), exact at 0, monotonic, bounded to ±1. */
inline float softClip (float x) noexcept
{
    x = clampf (x, -3.0f, 3.0f);
    const float x2 = x * x;
    return x * (27.0f + x2) / (27.0f + 9.0f * x2);
}

inline float fastTanh (float x) noexcept { return softClip (x); }

/** Replace NaN/Inf by zero (used at module boundaries so one bad module cannot poison a patch). */
inline float sanitize (float x) noexcept { return std::isfinite (x) ? x : 0.0f; }

inline void sanitizeBuffer (float* x, int n) noexcept
{
    for (int i = 0; i < n; ++i)
    {
        float v = x[i];
        if (! std::isfinite (v)) v = 0.0f;
        x[i] = clampf (v, -32.0f, 32.0f);
    }
}

/** Fast xorshift32 RNG; deterministic, RT safe. */
struct Rng
{
    uint32_t state = 0x9E3779B9u;
    explicit Rng (uint32_t seed = 0x9E3779B9u) noexcept : state (seed ? seed : 1u) {}
    uint32_t next() noexcept { state ^= state << 13; state ^= state >> 17; state ^= state << 5; return state; }
    float uniform() noexcept { return (float) (next() >> 8) * (1.0f / 16777216.0f); }          // [0,1)
    float bipolar() noexcept { return uniform() * 2.0f - 1.0f; }                                 // [-1,1)
};

/** One-pole smoother: y += (x - y) * a. */
struct OnePole
{
    float y = 0.0f, a = 1.0f;
    void setTime (float seconds, float sampleRate) noexcept
    {
        a = seconds <= 0.0f ? 1.0f : 1.0f - std::exp (-1.0f / (seconds * sampleRate));
    }
    float process (float x) noexcept { y += (x - y) * a; return y; }
    void reset (float v = 0.0f) noexcept { y = v; }
};

/** DC blocker. */
struct DcBlocker
{
    float x1 = 0.0f, y1 = 0.0f, r = 0.995f;
    void prepare (float sampleRate) noexcept { r = 1.0f - (2.0f * kPi * 10.0f / sampleRate); }
    float process (float x) noexcept { const float y = x - x1 + r * y1; x1 = x; y1 = y; return y; }
    void reset() noexcept { x1 = y1 = 0.0f; }
};

/** Topology-preserving state-variable filter (Simper/Zavalishin). */
struct Svf
{
    float ic1 = 0.0f, ic2 = 0.0f;
    float g = 0.0f, k = 1.0f, a1 = 0.0f, a2 = 0.0f, a3 = 0.0f;

    void set (float cutoffHz, float q, float sampleRate) noexcept
    {
        cutoffHz = clampf (cutoffHz, 5.0f, sampleRate * 0.49f);
        g = std::tan (kPi * cutoffHz / sampleRate);
        k = 1.0f / std::max (q, 0.05f);
        a1 = 1.0f / (1.0f + g * (g + k));
        a2 = g * a1;
        a3 = g * a2;
    }
    struct Out { float lp, bp, hp; };
    Out process (float v0) noexcept
    {
        const float v3 = v0 - ic2;
        const float v1 = a1 * ic1 + a2 * v3;
        const float v2 = ic2 + a2 * ic1 + a3 * v3;
        ic1 = 2.0f * v1 - ic1;
        ic2 = 2.0f * v2 - ic2;
        return { v2, v1, v0 - k * v1 - v2 };
    }
    void reset() noexcept { ic1 = ic2 = 0.0f; }
};

/** Power-of-two circular delay line with fractional (cubic) reads. Allocate in prepare only. */
struct DelayLine
{
    std::vector<float> buf;
    uint32_t mask = 0, w = 0;

    void allocate (int minSamples)
    {
        uint32_t n = 1;
        while ((int) n < minSamples + 4) n <<= 1;
        buf.assign (n, 0.0f);
        mask = n - 1;
        w = 0;
    }
    void clear() noexcept { if (! buf.empty()) std::memset (buf.data(), 0, buf.size() * sizeof (float)); w = 0; }
    int capacity() const noexcept { return (int) buf.size() - 4; }
    void push (float x) noexcept { buf[w] = x; w = (w + 1) & mask; }
    /** Read `d` samples behind the most recently pushed sample (d >= 0). */
    float readInt (int d) const noexcept { return buf[(w - 1 - (uint32_t) d) & mask]; }
    float read (float d) const noexcept
    {
        d = clampf (d, 1.0f, (float) capacity());
        const int di = (int) d;
        const float f = d - (float) di;
        const float xm1 = readInt (di - 1), x0 = readInt (di), x1 = readInt (di + 1), x2 = readInt (di + 2);
        const float c1 = 0.5f * (x1 - xm1);
        const float c2 = xm1 - 2.5f * x0 + 2.0f * x1 - 0.5f * x2;
        const float c3 = 0.5f * (x2 - xm1) + 1.5f * (x0 - x1);
        return ((c3 * f + c2) * f + c1) * f + x0;
    }
    float readLinear (float d) const noexcept
    {
        d = clampf (d, 0.0f, (float) capacity());
        const int di = (int) d;
        const float f = d - (float) di;
        return lerp (readInt (di), readInt (di + 1), f);
    }
};

/** PolyBLEP residual for band-limited steps. t = phase in [0,1), dt = phase increment. */
inline float polyBlep (float t, float dt) noexcept
{
    if (t < dt) { t /= dt; return t + t - t * t - 1.0f; }
    if (t > 1.0f - dt) { t = (t - 1.0f) / dt; return t * t + t + t + 1.0f; }
    return 0.0f;
}

/** Flush denormals to zero for the lifetime of the object (audio thread). */
class ScopedFlushDenormals
{
public:
    ScopedFlushDenormals() noexcept
    {
       #if WEFT_ARM64
        uint64_t fpcr;
        asm volatile ("mrs %0, fpcr" : "=r"(fpcr));
        saved = fpcr;
        asm volatile ("msr fpcr, %0" : : "r"(fpcr | (1ull << 24)));
       #elif WEFT_X86
        saved = _mm_getcsr();
        _mm_setcsr ((unsigned) saved | 0x8040u);
       #endif
    }
    ~ScopedFlushDenormals() noexcept
    {
       #if WEFT_ARM64
        asm volatile ("msr fpcr, %0" : : "r"(saved));
       #elif WEFT_X86
        _mm_setcsr ((unsigned) saved);
       #endif
    }
private:
    uint64_t saved = 0;
};

/** Tempo division helpers: index into a fixed table of note lengths (in beats). */
struct Division { const char* name; float beats; };
inline constexpr Division kDivisions[] = {
    { "1/64", 0.0625f }, { "1/32", 0.125f }, { "3/64", 0.1875f }, { "1/16", 0.25f }, { "3/32", 0.375f },
    { "1/8", 0.5f }, { "3/16", 0.75f }, { "1/4", 1.0f }, { "5/16", 1.25f }, { "3/8", 1.5f },
    { "1/2", 2.0f }, { "3/4", 3.0f }, { "1/1", 4.0f }, { "3/2", 6.0f }, { "2/1", 8.0f },
};
inline constexpr int kNumDivisions = (int) (sizeof (kDivisions) / sizeof (kDivisions[0]));
inline int divisionIndex (float v) noexcept { return (int) std::lround (clamp01 (v) * (float) (kNumDivisions - 1)); }
inline float divisionBeats (float v) noexcept { return kDivisions[divisionIndex (v)].beats; }

} // namespace weft::dsp
