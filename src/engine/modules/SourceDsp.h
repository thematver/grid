// SOURCE family private helpers: sine table, delayed polyBLEP/polyBLAMP residuals, interpolation,
// a radix-2 FFT (offline use: wavetable building, tests) and per-instance RNG seeding.
// Everything marked noexcept here is allocation-free and safe on the audio thread.
#pragma once

#include "../DspUtil.h"

#include <atomic>
#include <cmath>
#include <complex>
#include <cstdint>
#include <cstring>
#include <utility>

namespace weft::srcdsp
{
constexpr double kPiD = 3.14159265358979323846;

// ── sine table ──────────────────────────────────────────────────────────────────────────────────
struct SineTable
{
    static constexpr int kSize = 4096;   // linear interpolation error < 3e-7
    float v[kSize + 1];
    SineTable() noexcept
    {
        for (int i = 0; i <= kSize; ++i) v[i] = (float) std::sin (2.0 * kPiD * (double) i / (double) kSize);
    }
};
inline const SineTable kSineTable;

/** sin(2*pi*x) for any finite x (table lookup, linear interpolation). */
inline float sin2pi (float x) noexcept
{
    x -= std::floor (x);
    const float idx = x * (float) SineTable::kSize;
    int i = (int) idx;
    const float f = idx - (float) i;
    i &= SineTable::kSize - 1;
    return kSineTable.v[i] + (kSineTable.v[i + 1] - kSineTable.v[i]) * f;
}
inline float cos2pi (float x) noexcept { return sin2pi (x + 0.25f); }

// ── band-limited residuals with one sample of latency ("delayed" polyBLEP / polyBLAMP) ────────────
// A discontinuity happened `d` samples (0..1) before the current sample. `prev` is the previous,
// not yet emitted output sample, `cur` the current one; both receive their 2-point residual.

/** Step of height h (positive = upwards). */
inline void blepStep (float d, float h, float& prev, float& cur) noexcept
{
    const float e = 1.0f - d;
    prev += h * 0.5f * d * d;
    cur  -= h * 0.5f * e * e;
}

/** Slope change of c (output units per sample) — a corner. */
inline void blampCorner (float d, float c, float& prev, float& cur) noexcept
{
    const float e = 1.0f - d;
    prev += c * (1.0f / 6.0f) * d * d * d;
    cur  += c * (1.0f / 6.0f) * e * e * e;
}

// ── small helpers ─────────────────────────────────────────────────────────────────────────────────
/** Catmull-Rom (4-point cubic Hermite) interpolation, t in [0,1) between x0 and x1. */
inline float hermite (float xm1, float x0, float x1, float x2, float t) noexcept
{
    const float c1 = 0.5f * (x1 - xm1);
    const float c2 = xm1 - 2.5f * x0 + 2.0f * x1 - 0.5f * x2;
    const float c3 = 0.5f * (x2 - xm1) + 1.5f * (x0 - x1);
    return ((c3 * t + c2) * t + c1) * t + x0;
}

/** Cheap log2 for x > 0 (max error ~0.005), good enough for mip-level selection. */
inline float fastLog2 (float x) noexcept
{
    uint32_t bits;
    std::memcpy (&bits, &x, sizeof (bits));
    const float e = (float) ((int) ((bits >> 23) & 255u) - 127);
    bits = (bits & 0x007FFFFFu) | 0x3F800000u;
    float m;
    std::memcpy (&m, &bits, sizeof (m));
    return e + (-0.34484843f * m + 2.02466578f) * m - 0.67487759f;
}

inline float flushTiny (float x) noexcept { return std::abs (x) < 1.0e-20f ? 0.0f : x; }

/** Transparent below `knee`, then a smooth soft-clip that never exceeds 2 * knee. */
inline float kneeClip (float x, float knee) noexcept
{
    const float a = std::abs (x);
    if (a <= knee) return x;
    const float y = knee + knee * dsp::softClip ((a - knee) / knee);
    return x < 0.0f ? -y : y;
}

/** One-pole coefficient (y += (x - y) * a) for a cutoff in Hz. */
inline float onePoleCoef (float hz, float sampleRate) noexcept
{
    return 1.0f - std::exp (-2.0f * dsp::kPi * hz / sampleRate);
}

/** Distinct, well-mixed seeds for every instance (thread-safe). */
inline uint32_t nextInstanceSeed() noexcept
{
    static std::atomic<uint32_t> counter { 0x2545F491u };
    uint32_t x = counter.fetch_add (0x9E3779B9u, std::memory_order_relaxed);
    x ^= x >> 16; x *= 0x85EBCA6Bu; x ^= x >> 13; x *= 0xC2B2AE35u; x ^= x >> 16;
    return x ? x : 0x1234567u;
}

// ── FFT (offline only: allocates nothing, but is not meant for the audio thread) ─────────────────
/** In-place iterative radix-2 FFT. n must be a power of two. inverse=true uses e^{+j}, no 1/n scaling. */
inline void fft (std::complex<double>* x, int n, bool inverse)
{
    for (int i = 1, j = 0; i < n; ++i)
    {
        int bit = n >> 1;
        for (; j & bit; bit >>= 1) j ^= bit;
        j ^= bit;
        if (i < j) std::swap (x[i], x[j]);
    }
    for (int len = 2; len <= n; len <<= 1)
    {
        const double ang = 2.0 * kPiD / (double) len * (inverse ? 1.0 : -1.0);
        const int half = len >> 1;
        for (int k = 0; k < half; ++k)
        {
            const std::complex<double> w (std::cos (ang * k), std::sin (ang * k));
            for (int i = 0; i < n; i += len)
            {
                const std::complex<double> u = x[i + k], v = x[i + k + half] * w;
                x[i + k] = u + v;
                x[i + k + half] = u - v;
            }
        }
    }
}
} // namespace weft::srcdsp
