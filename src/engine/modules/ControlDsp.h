// CONTROL family — private helpers shared by the modulator implementations in Control.cpp.
// Everything here is allocation-free and safe to call on the audio thread.
#pragma once

#include "../DspUtil.h"
#include "../Module.h"
#include "../ParamMaps.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdint>

namespace weft::ctl
{
/** Processing granularity for coefficient updates (samples). Parameters may change every sample;
    rate / time coefficients are re-evaluated once per chunk, which is exact for constant values. */
constexpr int kChunk = 8;

/** NaN-safe clamp to [0,1] (NaN -> 0). Used before anything becomes an index. */
inline float safe01 (float v) noexcept { return v >= 0.0f ? (v <= 1.0f ? v : 1.0f) : 0.0f; }
inline int clampi (int v, int lo, int hi) noexcept { return v < lo ? lo : (v > hi ? hi : v); }

/** Floor modulo for possibly negative step counts. */
inline int64_t modFloor (int64_t k, int64_t n) noexcept { const int64_t r = k % n; return r < 0 ? r + n : r; }

/** Distinct deterministic seed per instance (constructors run on the message thread). */
inline uint32_t nextSeed() noexcept
{
    static std::atomic<uint32_t> counter { 0x2545F491u };
    uint32_t x = counter.fetch_add (0x9E3779B9u, std::memory_order_relaxed);
    x ^= x >> 16; x *= 0x85EBCA6Bu; x ^= x >> 13; x *= 0xC2B2AE35u; x ^= x >> 16;
    return x ? x : 1u;
}

/** Stateless 64 -> 32 bit hash (deterministic random choices locked to a step position). */
inline uint32_t hash32 (uint64_t k) noexcept
{
    k ^= k >> 33; k *= 0xFF51AFD7ED558CCDull; k ^= k >> 33; k *= 0xC4CEB9FE1A85EC53ull; k ^= k >> 33;
    return (uint32_t) k;
}

inline const Transport& transportOf (const ControlContext& c) noexcept
{
    static const Transport fallback {};
    return c.transport ? *c.transport : fallback;
}

inline double samplesPerBeat (const Transport& t) noexcept
{
    const double spb = (double) t.samplesPerBeat;
    return spb >= 1.0 && std::isfinite (spb) ? spb : 24000.0;
}

/** Beat position at the end of the sub-block (the time the returned control value refers to). */
inline double endPpq (const Transport& t, int n) noexcept
{
    const double p = std::isfinite (t.ppq) ? std::clamp (t.ppq, -1.0e12, 1.0e12) : 0.0;
    return p + (double) n / samplesPerBeat (t);
}

/** LOGIC / MATH input selector: 0..7 = modulator slot, 8..15 = macro. */
inline float inputValue (const ControlContext& c, float auxIndex) noexcept
{
    int i = auxIndex >= 0.0f ? (int) (std::min (auxIndex, 15.0f) + 0.5f) : 0;
    if (i < kNumSlots) return c.slotValues ? safe01 (c.slotValues[i]) : 0.0f;
    i -= kNumSlots;
    return (c.macroValues && i < kNumMacros) ? safe01 (c.macroValues[i]) : 0.0f;
}

/** Caches an expensive map (pow / exp) of a parameter value; recomputed only when the value changes. */
struct MapCache
{
    float key = -1.0f, value = 0.0f;
    template <typename F> float get (float v, F&& f) noexcept
    {
        if (v != key) { key = v; value = f (v); }
        return value;
    }
    void invalidate() noexcept { key = -1.0f; }
};

/** Cycles advanced over samples [i0, i1) at a free rate `hzOf(v)` (Hz), evaluated per chunk. */
template <typename F>
double freeCycles (const float* v, int i0, int i1, float sampleRate, MapCache& cache, F&& hzOf) noexcept
{
    double total = 0.0;
    for (int a = i0; a < i1;)
    {
        const int b = std::min (i1, (a & ~(kChunk - 1)) + kChunk);
        total += (double) cache.get (safe01 (v[(a + b - 1) >> 1]), hzOf) * (double) (b - a);
        a = b;
    }
    return total / (double) sampleRate;
}

/** Cycles advanced over [i0, i1) when one cycle lasts syncRateBeats(v) beats. */
inline double syncCycles (const float* v, int i0, int i1, double spb) noexcept
{
    double total = 0.0;
    for (int a = i0; a < i1;)
    {
        const int b = std::min (i1, (a & ~(kChunk - 1)) + kChunk);
        total += (double) (b - a) / (double) maps::syncRateBeats (safe01 (v[(a + b - 1) >> 1]));
        a = b;
    }
    return total / spb;
}

/** Step clock shared by RAND, SEQ and CLOCK: absolute step count plus position inside the step.
    Runs freely (advance) or is locked to the host position (lock). */
struct StepClock
{
    int64_t step = 0;
    double frac = 0.0;
    bool locked = false;

    enum Tick { None = 0, Next = 1, Jump = 2 };   // Next = natural step boundary, Jump = relocation

    void reset() noexcept { step = 0; frac = 0.0; locked = false; }

    Tick advance (double cycles) noexcept
    {
        locked = false;
        if (! (cycles > 0.0)) return None;
        frac += std::min (cycles, 1.0e6);
        if (frac < 1.0) return None;
        const double w = std::floor (frac);
        frac -= w;
        if (frac >= 1.0 || frac < 0.0) frac = 0.0;
        step += (int64_t) w;
        return Next;
    }

    Tick lock (double pos) noexcept
    {
        pos = std::isfinite (pos) ? std::clamp (pos, -1.0e15, 1.0e15) : 0.0;
        const double fl = std::floor (pos);
        const int64_t s = (int64_t) fl;
        double f = pos - fl;
        if (f >= 1.0 || f < 0.0) f = 0.0;
        Tick t = None;
        if (s != step) t = (s == step + 1) ? Next : Jump;
        step = s; frac = f; locked = true;
        return t;
    }
};

/** Note events received between two process() calls, replayed at their sample offsets. */
struct NoteEvents
{
    bool on = false, off = false, offLast = false;

    void noteOn() noexcept  { on = true;  offLast = false; }
    void noteOff() noexcept { off = true; offLast = true; }
    void clear() noexcept   { on = off = offLast = false; }

    struct Event { int at = 0; bool isOn = false; };
    struct Schedule
    {
        Event e[2];
        int count = 0;
        int onAt = -1;      // offset of the (last) note-on, -1 if none
        int offAt = -1;
    };

    /** Consumes the pending events. Offsets come from the note state when inside the sub-block,
        otherwise 0; events at the same offset keep the order in which they were received. */
    Schedule take (const ControlContext& c) noexcept
    {
        Schedule s;
        const int n = c.numSamples;
        auto at = [n] (int o) { return o >= 0 && o < n ? o : 0; };
        if (on)  s.onAt  = at (c.note ? c.note->trigger : -1);
        if (off) s.offAt = at (c.note ? c.note->release : -1);
        if (on && off)
        {
            // the event received last goes second when both land on the same sample
            const bool offSecond = s.offAt > s.onAt || (s.offAt == s.onAt && offLast);
            s.e[0] = offSecond ? Event { s.onAt, true } : Event { s.offAt, false };
            s.e[1] = offSecond ? Event { s.offAt, false } : Event { s.onAt, true };
            s.count = 2;
        }
        else if (on)  { s.e[0] = { s.onAt, true };   s.count = 1; }
        else if (off) { s.e[0] = { s.offAt, false }; s.count = 1; }
        clear();
        return s;
    }
};

/** Bjorklund (Toussaint) euclidean rhythm: bit i set = step i carries a pulse. 1..32 steps. */
inline uint32_t bjorklund (int steps, int pulses) noexcept
{
    steps = clampi (steps, 1, 32);
    pulses = clampi (pulses, 0, steps);
    if (pulses == 0) return 0u;
    if (pulses == steps) return steps == 32 ? 0xffffffffu : ((1u << steps) - 1u);

    struct Group { uint32_t bits; int len; };
    Group a[32], b[32], ta[32], tb[32];
    int na = pulses, nb = steps - pulses;
    for (int i = 0; i < na; ++i) a[i] = { 1u, 1 };
    for (int i = 0; i < nb; ++i) b[i] = { 0u, 1 };
    while (nb > 1)
    {
        const int m = std::min (na, nb);
        int ca = 0, cb = 0;
        for (int i = 0; i < m; ++i)  ta[ca++] = { a[i].bits | (b[i].bits << a[i].len), a[i].len + b[i].len };
        for (int i = m; i < na; ++i) tb[cb++] = a[i];
        for (int i = m; i < nb; ++i) tb[cb++] = b[i];
        for (int i = 0; i < ca; ++i) a[i] = ta[i];
        for (int i = 0; i < cb; ++i) b[i] = tb[i];
        na = ca; nb = cb;
    }
    uint32_t out = 0u;
    int pos = 0;
    for (int i = 0; i < na; ++i) { out |= a[i].bits << pos; pos += a[i].len; }
    for (int i = 0; i < nb; ++i) { out |= b[i].bits << pos; pos += b[i].len; }
    return out;
}

/** Catmull-Rom interpolation between p1 and p2 (t in [0,1]). */
inline float catmullRom (float p0, float p1, float p2, float p3, float t) noexcept
{
    const float a = -0.5f * p0 + 1.5f * p1 - 1.5f * p2 + 0.5f * p3;
    const float b = p0 - 2.5f * p1 + 2.0f * p2 - 0.5f * p3;
    const float c = 0.5f * (p2 - p0);
    return ((a * t + b) * t + c) * t + p1;
}

/** sin(x) for |x| <= ~pi/2 (odd polynomial, error < 1e-6 on that range). */
inline float sinSmall (float x) noexcept
{
    const float x2 = x * x;
    return x * (1.0f + x2 * (-1.0f / 6.0f + x2 * (1.0f / 120.0f + x2 * (-1.0f / 5040.0f + x2 * (1.0f / 362880.0f)))));
}

/** One-pole coefficient so that a step response covers 1 - 1/e per `seconds` (block of `samples`). */
inline float onePoleCoef (float seconds, float samples, float sampleRate) noexcept
{
    if (! (seconds > 1.0e-6f)) return 1.0f;
    return 1.0f - std::exp (-samples / (seconds * sampleRate));
}

} // namespace weft::ctl
