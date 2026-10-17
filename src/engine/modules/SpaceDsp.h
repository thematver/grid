// SPACE family helpers (delay lines, allpasses, windows, saturators). Private to modules/Space.cpp.
// Everything here is allocation-free except the allocate() methods, which run from prepare().
#pragma once

#include "../DspUtil.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <vector>

namespace weft::space
{
using dsp::clamp01;
using dsp::clampf;
using dsp::kPi;
using dsp::kTwoPi;
using dsp::softClip;

inline int clampi (int x, int lo, int hi) noexcept { return x < lo ? lo : (x > hi ? hi : x); }

/** sin (2πp) for any finite p: parabola plus one refinement step, |error| < 1.1e-3. */
inline float sin2pi (float p) noexcept
{
    p -= std::floor (p);
    const float x = 2.0f * p - 1.0f;                // sin (2πp) = −sin (πx), x in [−1, 1)
    float y = 4.0f * x * (1.0f - std::abs (x));
    y += 0.225f * (y * std::abs (y) - y);
    return -y;
}

/** Hann window sin²(πt) for t in [0, 1] (Bhaskara's sine approximation, |error| < 3e-3). */
inline float hann (float t) noexcept
{
    t = clamp01 (t);
    const float u = t * (1.0f - t);
    const float s = 16.0f * u / (5.0f - 4.0f * u);
    return s * s;
}

/** Identity up to `knee`, then a C1-continuous soft approach to `ceiling`. Bounded by ±ceiling,
    and maps NaN to ±ceiling so a bad value can never circulate in a feedback loop. */
inline float kneeSat (float x, float knee, float ceiling) noexcept
{
    const float a = std::abs (x);
    if (a <= knee) return x;
    const float r = ceiling - knee;
    const float z = std::fmin ((a - knee) / r, 3.0f);
    return std::copysign (knee + r * softClip (z), x);
}

/** Tape-like saturation with unity small-signal gain, bounded to ±1/drive. */
inline float tapeSat (float x, float drive) noexcept { return softClip (x * drive) / drive; }

inline float flushTiny (float x) noexcept { return std::abs (x) < 1.0e-20f ? 0.0f : x; }

/** Near equal-power dry/wet law: 0 -> dry, 1 -> wet, 0.5 -> both at 0.75 (≈ −2.5 dB). */
struct MixGains { float dry, wet; };
inline MixGains mixLaw (float m) noexcept
{
    m = clamp01 (m);
    return { 1.0f - m * m, m * (2.0f - m) };
}

/** Topology-preserving one-pole low-pass; the coefficient may be shared between instances. */
struct Tpt1
{
    float s = 0.0f;
    static float coef (float hz, float sampleRate) noexcept
    {
        const float g = std::tan (kPi * clampf (hz, 1.0f, 0.49f * sampleRate) / sampleRate);
        return g / (1.0f + g);
    }
    float lp (float x, float G) noexcept
    {
        const float v = (x - s) * G;
        const float y = v + s;
        s = y + v;
        return y;
    }
    void reset() noexcept { s = 0.0f; }
    void flush() noexcept { s = flushTiny (s); }
};

/** Power-of-two circular buffer. Reads are relative to the sample about to be written, so with
    read-before-write `at (d)` / `cubic (d)` return the input delayed by exactly d samples. */
struct Ring
{
    std::vector<float> buf;
    uint32_t mask = 0, w = 0;

    void allocate (int minSamples)
    {
        uint32_t n = 16;
        while ((int64_t) n < (int64_t) minSamples + 8) n <<= 1;
        buf.assign (n, 0.0f);
        mask = n - 1;
        w = 0;
    }
    bool empty() const noexcept { return buf.empty(); }
    void clear() noexcept
    {
        if (! buf.empty()) std::memset (buf.data(), 0, buf.size() * sizeof (float));
        w = 0;
    }
    float maxDelay() const noexcept { return (float) mask - 4.0f; }
    void write (float x) noexcept { buf[w] = x; w = (w + 1) & mask; }
    /** Sample written `d` writes ago (d = 1: the most recent one). */
    float at (uint32_t d) const noexcept { return buf[(w - d) & mask]; }
    float linear (float d) const noexcept
    {
        d = clampf (d, 1.0f, maxDelay());
        const uint32_t di = (uint32_t) d;
        const float f = d - (float) di;
        const float a = at (di), b = at (di + 1);
        return a + (b - a) * f;
    }
    /** 4-point cubic Hermite read, d >= 2. */
    float cubic (float d) const noexcept
    {
        d = clampf (d, 2.0f, maxDelay());
        const uint32_t di = (uint32_t) d;
        const float f = d - (float) di;
        const float xm1 = at (di - 1), x0 = at (di), x1 = at (di + 1), x2 = at (di + 2);
        const float c1 = 0.5f * (x1 - xm1);
        const float c2 = xm1 - 2.5f * x0 + 2.0f * x1 - 0.5f * x2;
        const float c3 = 0.5f * (x2 - xm1) + 1.5f * (x0 - x1);
        return ((c3 * f + c2) * f + c1) * f + x0;
    }
};

/** Schroeder allpass (lattice form) with an integer delay inside a pre-allocated buffer. */
struct Allpass
{
    std::vector<float> buf;
    int size = 2, pos = 0, delay = 1;

    void allocate (int maxDelay)
    {
        size = std::max (2, maxDelay + 1);
        buf.assign ((size_t) size, 0.0f);
        pos = 0;
        delay = clampi (delay, 1, size - 1);
    }
    void setDelay (int d) noexcept { delay = clampi (d, 1, size - 1); }
    void clear() noexcept
    {
        if (! buf.empty()) std::memset (buf.data(), 0, buf.size() * sizeof (float));
        pos = 0;
    }
    float process (float x, float g) noexcept
    {
        int rp = pos - delay;
        if (rp < 0) rp += size;
        const float vd = buf[(size_t) rp];
        const float v = x + g * vd;
        buf[(size_t) pos] = v;
        if (++pos >= size) pos = 0;
        return vd - g * v;
    }
};

/** De-clicks discrete mode changes: ramps a gain to zero, switches, ramps back up. */
struct ModeFade
{
    int current = -1;
    float gain = 1.0f, step = 1.0f / 192.0f;

    void prepare (float sampleRate) noexcept { step = 1.0f / std::max (1.0f, 0.004f * sampleRate); }
    void reset() noexcept { current = -1; gain = 1.0f; }
    /** Advance one sample towards `requested`; returns true when the mode switched on this sample
        (the faded signal is silent at that point, or this is the first call after reset). */
    bool tick (int requested) noexcept
    {
        if (current < 0) { current = requested; gain = 1.0f; return true; }
        if (requested != current)
        {
            gain -= step;
            if (gain <= 0.0f) { gain = 0.0f; current = requested; return true; }
        }
        else if (gain < 1.0f) gain = std::min (1.0f, gain + step);
        return false;
    }
};

/** Normalized fast Walsh–Hadamard transform of 8 values (orthonormal, energy preserving). */
inline void hadamard8 (float* x) noexcept
{
    for (int i = 0; i < 8; i += 2) { const float a = x[i], b = x[i + 1]; x[i] = a + b; x[i + 1] = a - b; }
    for (int i = 0; i < 8; i += 4)
        for (int j = 0; j < 2; ++j) { const float a = x[i + j], b = x[i + j + 2]; x[i + j] = a + b; x[i + j + 2] = a - b; }
    for (int j = 0; j < 4; ++j) { const float a = x[j], b = x[j + 4]; x[j] = a + b; x[j + 4] = a - b; }
    constexpr float k = 0.35355339059327373f;   // 1 / sqrt (8)
    for (int i = 0; i < 8; ++i) x[i] *= k;
}

} // namespace weft::space
