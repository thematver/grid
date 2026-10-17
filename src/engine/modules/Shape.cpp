// SHAPE family: FILTER, FOLD, DRIVE, RING, COMB and FORMANT. Effects process io in place.
#include "../Module.h"
#include "../ModuleSpec.h"
#include "../ParamMaps.h"
#include "SourceDsp.h"

#include <array>
#include <cmath>
#include <limits>

namespace weft
{
namespace
{
using namespace dsp;
using srcdsp::kneeClip;
using srcdsp::flushTiny;

/** tan(x) for 0 <= x < ~1.56 (Padé 7/6, relative error < 1e-5 below 1.4). */
inline float fastTan (float x) noexcept
{
    const float x2 = x * x;
    return x * (135135.0f + x2 * (-17325.0f + x2 * (378.0f - x2))) / (135135.0f + x2 * (-62370.0f + x2 * (3150.0f - 28.0f * x2)));
}

/** Prewarped SVF gain g for a cutoff, clamped below Nyquist. */
inline float svfG (float hz, float sampleRate) noexcept
{
    const float x = kPi * clampf (hz, 8.0f, 0.49f * sampleRate) / sampleRate;
    return fastTan (std::min (x, 1.5394f));
}

struct Cached
{
    float key = std::numeric_limits<float>::quiet_NaN(), value = 0.0f;
    template <typename F> float get (float v, F&& f) noexcept { if (! (v == key)) { key = v; value = f (v); } return value; }
    void invalidate() noexcept { key = std::numeric_limits<float>::quiet_NaN(); }
};

/** Equal-ish power dry/wet. */
inline void mixGains (float m, float& dry, float& wet) noexcept { m = clamp01 (m); dry = 1.0f - m * m; wet = m * (2.0f - m); }

/** One-pole TPT low-pass. */
struct Lp1
{
    float s = 0.0f;
    static float coef (float hz, float sr) noexcept { const float g = svfG (hz, sr); return g / (1.0f + g); }
    float process (float x, float G) noexcept { const float v = (x - s) * G; const float y = v + s; s = y + v; return y; }
};

// ══ FILTER ══════════════════════════════════════════════════════════════════════════════════════

/** Simper SVF stage with gently saturating integrators. */
struct SvfStage
{
    float ic1 = 0.0f, ic2 = 0.0f;
    inline void tick (float v0, float g, float k, float& lp, float& bp, float& hp) noexcept
    {
        const float a1 = 1.0f / (1.0f + g * (g + k));
        const float a2 = g * a1, a3 = g * a2;
        const float v3 = v0 - ic2;
        const float v1 = a1 * ic1 + a2 * v3;
        const float v2 = ic2 + a2 * ic1 + a3 * v3;
        ic1 = kneeClip (2.0f * v1 - ic1, 1.2f);
        ic2 = kneeClip (2.0f * v2 - ic2, 1.2f);
        lp = v2; bp = v1; hp = v0 - k * v1 - v2;
    }
    void flush() noexcept { ic1 = flushTiny (ic1); ic2 = flushTiny (ic2); }
};

class Filter final : public AudioModule
{
public:
    void prepare (double sr) override { rate = (float) sr; reset(); }
    void reset() override { for (auto& c : st) for (auto& s : c) s = {}; }

    void process (const ProcessContext& c, const ParamInputs& p, float* const* io, int nch) override
    {
        const int type = std::clamp (p.mode[0], 0, 3);
        const bool two = p.mode[1] == 1;
        const float keyOct = (c.note->pitch - 60.0f) / 12.0f;
        for (int i = 0; i < c.numSamples; ++i)
        {
            const float cut = clamp01 (p.v[0][i]), res = clamp01 (p.v[1][i]), drive = clamp01 (p.v[2][i]), key = clamp01 (p.v[3][i]);
            const float hz = 20.0f * std::exp2 (9.965784f * cut + keyOct * key);
            const float g = gC.get (hz, [this] (float f) { return svfG (f, rate); });
            const float q = qC.get (res, [] (float r) { return maps::filterQ (r); });
            // beyond 96% resonance the damping goes slightly negative: musical self-oscillation, bounded by the integrators
            const float k = 1.0f / q - std::max (0.0f, (res - 0.96f) * 0.75f);
            const float dg = drive > 0.0f ? dC.get (drive, [] (float d) { return dbToGain (24.0f * d); }) : 1.0f;
            const float comp = drive > 0.0f ? cC.get (drive, [] (float d) { return 1.0f / std::pow (dbToGain (24.0f * d), 0.6f); }) : 1.0f;
            for (int ch = 0; ch < nch; ++ch)
            {
                float x = io[ch][i];
                if (drive > 0.0f) x = 2.0f * softClip (0.5f * x * dg) * comp;
                float lp, bp, hp;
                float y = x;
                for (int s = 0; s < (two ? 2 : 1); ++s)
                {
                    st[ch][s].tick (y, g, k, lp, bp, hp);
                    switch (type)
                    {
                        case 0: y = lp; break;
                        case 1: y = bp * std::max (k, 0.025f); break;
                        case 2: y = hp; break;
                        default: y = lp + hp; break;
                    }
                }
                io[ch][i] = kneeClip (y, 1.0f);
            }
        }
        for (auto& ch : st) for (auto& s : ch) s.flush();
    }

private:
    float rate = 48000.0f;
    SvfStage st[2][2];
    Cached gC, qC, dC, cC;
};

// ══ FOLD ════════════════════════════════════════════════════════════════════════════════════════

/** First-order antiderivative anti-aliased sine folder: f(x) = sin(pi x / 2). */
struct AdaaFold
{
    float x1 = 0.0f;
    static float f (float x) noexcept  { return srcdsp::sin2pi (x * 0.25f); }
    static float F (float x) noexcept  { return -0.63661977f * srcdsp::cos2pi (x * 0.25f); }
    float process (float x) noexcept
    {
        const float d = x - x1;
        float y;
        if (std::abs (d) < 1.0e-3f) y = f (0.5f * (x + x1));
        else y = (F (x) - F (x1)) / d;
        x1 = x;
        return y;
    }
};

class Fold final : public AudioModule
{
public:
    void prepare (double sr) override
    {
        rate = (float) sr;
        envRel = std::exp (-1.0f / (0.12f * rate));
        reset();
    }
    void reset() override { for (auto& s : st) s = {}; }

    void process (const ProcessContext& c, const ParamInputs& p, float* const* io, int nch) override
    {
        const int stages = std::clamp (p.mode[0], 0, 3) + 1;
        for (int i = 0; i < c.numSamples; ++i)
        {
            const float G = gC.get (p.v[0][i], [] (float v) { return maps::foldGain (v); });
            const float bias = maps::bipolar (p.v[1][i]) * 0.8f;
            const float tone = clamp01 (p.v[2][i]);
            const float lpG = tone < 0.999f ? tC.get (tone, [this] (float t) { return Lp1::coef (300.0f * std::pow (66.0f, t), rate); }) : 1.0f;
            float dry, wet;
            mixGains (p.v[3][i], dry, wet);
            const float later = 1.0f + 0.6f * (G - 1.0f);
            for (int ch = 0; ch < nch; ++ch)
            {
                auto& s = st[ch];
                const float x = io[ch][i];
                const float a = std::abs (x);
                s.env = a > s.env ? a : s.env * envRel + a * (1.0f - envRel);
                const float e = std::max (s.env, 0.02f);
                float y = s.fold[0].process (x / e * G + bias);
                for (int k = 1; k < stages; ++k) y = s.fold[k].process (y * later);
                y = s.dc.process (y) * e;
                if (lpG < 1.0f) y = s.lp.process (y, lpG);
                io[ch][i] = x * dry + y * wet;
            }
        }
        for (auto& s : st) { s.lp.s = flushTiny (s.lp.s); s.env = flushTiny (s.env); }
    }

private:
    struct State { AdaaFold fold[4]; DcBlocker dc; Lp1 lp; float env = 0.0f; };
    State st[2];
    float rate = 48000.0f, envRel = 0.999f;
    Cached gC, tC;
};

// ══ DRIVE ═══════════════════════════════════════════════════════════════════════════════════════

/** ADAA saturators. f = algebraic sigmoid (tape / tube) or hard clip (diode). */
struct AdaaSat
{
    float x1 = 0.0f;
    static float fSig (float x) noexcept { return x / std::sqrt (1.0f + x * x); }
    static float FSig (float x) noexcept { return std::sqrt (1.0f + x * x); }
    static float fHard (float x) noexcept { return clampf (x, -1.0f, 1.0f); }
    static float FHard (float x) noexcept { const float a = std::abs (x); return a <= 1.0f ? 0.5f * x * x : a - 0.5f; }

    float sig (float x) noexcept
    {
        const float d = x - x1;
        const float y = std::abs (d) < 1.0e-4f ? fSig (0.5f * (x + x1)) : (FSig (x) - FSig (x1)) / d;
        x1 = x;
        return y;
    }
    float hard (float x) noexcept
    {
        const float d = x - x1;
        const float y = std::abs (d) < 1.0e-4f ? fHard (0.5f * (x + x1)) : (FHard (x) - FHard (x1)) / d;
        x1 = x;
        return y;
    }
};

class Drive final : public AudioModule
{
public:
    void prepare (double sr) override
    {
        rate = (float) sr;
        bumpG = Lp1::coef (110.0f, rate);
        hfG = Lp1::coef (13000.0f, rate);
        splitG = Lp1::coef (1200.0f, rate);
        reset();
    }
    void reset() override { for (auto& s : st) s = {}; tC.invalidate(); }

    void process (const ProcessContext& c, const ParamInputs& p, float* const* io, int nch) override
    {
        const int type = std::clamp (p.mode[0], 0, 3);
        for (int i = 0; i < c.numSamples; ++i)
        {
            const float g = gC.get (p.v[0][i], [] (float v) { return dbToGain (maps::driveGainDb (v)); });
            float dry, wet;
            mixGains (p.v[3][i], dry, wet);
            if (type == 3)
            {
                const float hold = std::pow (64.0f, clamp01 (p.v[1][i]));
                const float steps = std::exp2 (maps::crushBits (p.v[2][i]) - 1.0f);
                for (int ch = 0; ch < nch; ++ch)
                {
                    auto& s = st[ch];
                    const float x = io[ch][i];
                    s.count -= 1.0f;
                    if (s.count <= 0.0f) { s.held = std::round (clampf (x * g, -1.0f, 1.0f) * steps) / steps; s.count += hold; }
                    io[ch][i] = x * dry + s.held * std::min (1.0f, 1.0f / std::sqrt (g)) * wet;
                }
                continue;
            }
            const float tone = clamp01 (p.v[1][i]);
            const float bias = clamp01 (p.v[2][i]);
            const float tb = 0.08f + 0.6f * bias;
            const float makeup = mC.get (g * 7.0f + (float) type + bias * 0.01f, [&] (float)
            {
                // output of a 0.5 peak sine at this gain, normalised back to 0.5
                float pk = 0.0f;
                for (int j = 0; j < 64; ++j)
                {
                    const float x = 0.5f * std::sin (kTwoPi * (float) j / 64.0f) * g;
                    float y;
                    if (type == 0) y = AdaaSat::fSig (x);
                    else if (type == 1) y = AdaaSat::fSig (x + tb) - AdaaSat::fSig (tb);
                    else y = AdaaSat::fHard (x);
                    pk = std::max (pk, std::abs (y));
                }
                return 0.5f / std::max (1.0e-4f, pk);
            });
            // tone: below 0.5 a closing low-pass, above a high shelf (up to +6 dB)
            const float lpG = tone < 0.5f ? tC.get (tone, [this] (float t) { return Lp1::coef (700.0f * std::pow (28.0f, t * 2.0f), rate); }) : 1.0f;
            const float shelf = tone > 0.5f ? dbToGain ((tone - 0.5f) * 12.0f) : 1.0f;
            for (int ch = 0; ch < nch; ++ch)
            {
                auto& s = st[ch];
                const float x = io[ch][i];
                const float xi = x * g;
                float y;
                if (type == 0)
                {
                    const float low = s.bump.process (xi, bumpG);
                    y = s.sat.sig (xi + 0.18f * low);
                    y = s.hf.process (y, hfG) * 0.92f;
                }
                else if (type == 1) y = s.sat.sig (xi + tb) - AdaaSat::fSig (tb);
                else y = s.sat.hard (xi * 1.2f) * 0.85f;
                y = s.dc.process (y * makeup);
                if (lpG < 1.0f) y = s.tone.process (y, lpG);
                else if (shelf > 1.0f) { const float lo = s.split.process (y, splitG); y = lo + (y - lo) * shelf; }
                io[ch][i] = x * dry + y * wet;
            }
        }
        for (auto& s : st) { s.bump.s = flushTiny (s.bump.s); s.hf.s = flushTiny (s.hf.s); s.tone.s = flushTiny (s.tone.s); s.split.s = flushTiny (s.split.s); }
    }

private:
    struct State { AdaaSat sat; Lp1 bump, hf, tone, split; DcBlocker dc; float held = 0.0f, count = 0.0f; };
    State st[2];
    float rate = 48000.0f, bumpG = 0.01f, hfG = 0.5f, splitG = 0.1f;
    Cached gC, mC, tC;
};

// ══ RING ════════════════════════════════════════════════════════════════════════════════════════

/** 90° phase-difference network (two chains of four 2nd-order allpasses, Olli Niemitalo). */
struct Hilbert
{
    struct Ap { float c, x1 = 0, x2 = 0, y1 = 0, y2 = 0; float process (float x) noexcept { const float y = c * (x + y2) - x2; x2 = x1; x1 = x; y2 = y1; y1 = y; return y; } };
    Ap a[4] { { 0.6923878f * 0.6923878f }, { 0.9360654322959f * 0.9360654322959f }, { 0.9882295226860f * 0.9882295226860f }, { 0.9987488452737f * 0.9987488452737f } };
    Ap b[4] { { 0.4021921162426f * 0.4021921162426f }, { 0.8561710882420f * 0.8561710882420f }, { 0.9722909545651f * 0.9722909545651f }, { 0.9952884791278f * 0.9952884791278f } };
    float delayed = 0.0f;
    void process (float x, float& re, float& im) noexcept
    {
        float ya = x, yb = x;
        for (auto& f : a) ya = f.process (ya);
        for (auto& f : b) yb = f.process (yb);
        re = delayed; delayed = ya;
        im = yb;
    }
    void reset() noexcept { for (auto& f : a) f.x1 = f.x2 = f.y1 = f.y2 = 0; for (auto& f : b) f.x1 = f.x2 = f.y1 = f.y2 = 0; delayed = 0; }
    void flush() noexcept { for (auto& f : a) { f.y1 = flushTiny (f.y1); f.y2 = flushTiny (f.y2); } for (auto& f : b) { f.y1 = flushTiny (f.y1); f.y2 = flushTiny (f.y2); } }
};

class Ring final : public AudioModule
{
public:
    void prepare (double sr) override { rate = (float) sr; reset(); }
    void reset() override { ph[0] = 0.0; ph[1] = 0.25; hb[0].reset(); hb[1].reset(); }

    void process (const ProcessContext& c, const ParamInputs& p, float* const* io, int nch) override
    {
        const bool shift = p.mode[0] == 1;
        const bool key = p.mode[1] == 1;
        const float noteHz = noteToHz (c.note->pitch);
        for (int i = 0; i < c.numSamples; ++i)
        {
            const float fv = clamp01 (p.v[0][i]);
            float dry, wet;
            mixGains (p.v[1][i], dry, wet);
            const float fine = maps::bipolar (p.v[2][i]);
            const float spread = clamp01 (p.v[3][i]);
            float hz;
            if (shift)
            {
                const float b = maps::bipolar (fv);
                hz = (b < 0.0f ? -1.0f : 1.0f) * b * b * 2000.0f * std::exp2 (fine / 12.0f);
            }
            else hz = (key ? maps::ringRatio (fv) * noteHz : maps::ringFreqHz (fv)) * std::exp2 (fine / 12.0f);
            for (int ch = 0; ch < nch; ++ch)
            {
                const float chHz = ch == 1 ? hz * (1.0f + 0.03f * spread) : hz;
                ph[ch] += (double) (chHz / rate);
                ph[ch] -= std::floor (ph[ch]);
                const float phase = (float) ph[ch] + (ch == 1 ? 0.25f * spread : 0.0f);
                const float x = io[ch][i];
                float y;
                if (shift)
                {
                    float re, im;
                    hb[ch].process (x, re, im);
                    y = re * srcdsp::cos2pi (phase) + im * srcdsp::sin2pi (phase);
                }
                else y = x * srcdsp::sin2pi (phase);
                io[ch][i] = x * dry + y * wet;
            }
        }
        hb[0].flush(); hb[1].flush();
    }

private:
    float rate = 48000.0f;
    double ph[2] { 0.0, 0.25 };
    Hilbert hb[2];
};

// ══ COMB ════════════════════════════════════════════════════════════════════════════════════════

class Comb final : public AudioModule
{
public:
    void prepare (double sr) override
    {
        rate = (float) sr;
        for (auto& l : line) l.allocate ((int) (rate / 18.0f) + 8);
        reset();
    }
    void reset() override { for (auto& l : line) l.clear(); for (auto& s : lp) s = 0.0f; for (auto& d : dc) d.reset(); delay = -1.0f; }

    void process (const ProcessContext& c, const ParamInputs& p, float* const* io, int nch) override
    {
        const int tuning = std::clamp (p.mode[0], 0, 2);
        const float sign = p.mode[1] == 1 ? -1.0f : 1.0f;
        const int n = c.numSamples;
        for (int i = 0; i < n; ++i)
        {
            const float tv = clamp01 (p.v[0][i]);
            const float damp = clamp01 (p.v[2][i]);
            const float key = tv + damp * 3.1f + (tuning == 2 ? c.note->pitch * 7.0f : 0.0f) + (float) tuning * 11.0f;
            if (! (key == lastKey))
            {
                lastKey = key;
                float hz;
                if (tuning == 0) hz = noteToHz ((float) maps::combNote (tv));
                else if (tuning == 1) hz = maps::combFreeHz (tv);
                else hz = noteToHz (c.note->pitch + maps::semis24 (tv));
                hz = clampf (hz, 20.0f, rate * 0.3f);
                a = 0.85f * damp * damp;
                const float w = kTwoPi * hz / rate;
                const float pd = std::atan2 (a * std::sin (w), 1.0f - a * std::cos (w)) / w;
                target = clampf (rate / hz - pd, 2.0f, (float) line[0].capacity() - 4.0f);
                if (delay < 0.0f) delay = target;
            }
            delay += (target - delay) * 0.01f;
            const float fb = maps::combFeedback (p.v[1][i]) * sign;
            const float norm = std::sqrt (1.0f - fb * fb) * 1.2f + 0.05f;
            float dry, wet;
            mixGains (p.v[3][i], dry, wet);
            for (int ch = 0; ch < nch; ++ch)
            {
                const float x = io[ch][i];
                const float d = line[ch].read (delay - 1.0f);
                lp[ch] = (1.0f - a) * d + a * lp[ch];
                line[ch].push (kneeClip (x + lp[ch] * fb, 1.5f));
                const float y = dc[ch].process (d) * norm;
                io[ch][i] = x * dry + kneeClip (y, 1.5f) * wet;
            }
        }
        lp[0] = flushTiny (lp[0]); lp[1] = flushTiny (lp[1]);
    }

private:
    DelayLine line[2];
    DcBlocker dc[2];
    float lp[2] {};
    float rate = 48000.0f, a = 0.0f, delay = -1.0f, target = 100.0f, lastKey = -1.0f;
};

// ══ FORMANT ═════════════════════════════════════════════════════════════════════════════════════

class Formant final : public AudioModule
{
public:
    void prepare (double sr) override { rate = (float) sr; reset(); }
    void reset() override { for (auto& ch : st) for (auto& s : ch) s = {}; }

    void process (const ProcessContext& c, const ParamInputs& p, float* const* io, int nch) override
    {
        const int voice = p.mode[0] == 1 ? 1 : 0;
        for (int i = 0; i < c.numSamples; ++i)
        {
            const float vowel = clamp01 (p.v[0][i]) * 4.0f;
            const float res = clamp01 (p.v[1][i]);
            const float shift = std::exp2 (maps::semis12 (p.v[2][i]) / 12.0f);
            float dry, wet;
            mixGains (p.v[3][i], dry, wet);
            const int v0 = std::min (3, (int) vowel);
            const float t = vowel - (float) v0;
            const float q = 3.0f + 17.0f * res;
            const float k = 1.0f / q;
            const float makeup = 2.2f * std::sqrt (q);
            float g[kF], gain[kF];
            for (int f = 0; f < kF; ++f)
            {
                const float hz = lerp (kTable[voice][v0][f], kTable[voice][v0 + 1][f], t) * shift;
                g[f] = svfG (hz, rate);
                gain[f] = lerp (kGain[v0][f], kGain[v0 + 1][f], t);
            }
            for (int ch = 0; ch < nch; ++ch)
            {
                const float x = io[ch][i];
                float y = 0.0f;
                for (int f = 0; f < kF; ++f)
                {
                    float lp, bp, hp;
                    st[ch][f].tick (x, g[f], k, lp, bp, hp);
                    y += bp * k * gain[f];
                }
                io[ch][i] = x * dry + kneeClip (y * makeup, 2.0f) * wet;
            }
        }
        for (auto& ch : st) for (auto& s : ch) s.flush();
    }

private:
    static constexpr int kF = 4;
    // [voice LOW/HIGH][vowel A E I O U][formant]
    static constexpr float kTable[2][5][kF] = {
        { { 730, 1090, 2440, 3400 }, { 530, 1840, 2480, 3500 }, { 270, 2290, 3010, 3700 }, { 570, 840, 2410, 3300 }, { 300, 870, 2240, 3300 } },
        { { 850, 1220, 2810, 4000 }, { 610, 2330, 2990, 4100 }, { 310, 2790, 3310, 4300 }, { 590, 920, 2710, 3900 }, { 370, 950, 2670, 3900 } } };
    static constexpr float kGain[5][kF] = {
        { 1.0f, 0.55f, 0.32f, 0.12f }, { 1.0f, 0.42f, 0.36f, 0.12f }, { 0.8f, 0.55f, 0.5f, 0.2f }, { 1.0f, 0.6f, 0.2f, 0.1f }, { 1.0f, 0.4f, 0.15f, 0.08f } };
    SvfStage st[2][kF];
    float rate = 48000.0f;
};

} // namespace

std::unique_ptr<AudioModule> createShapeModule (ModuleType t)
{
    switch (t)
    {
        case ModuleType::Filter:  return std::make_unique<Filter>();
        case ModuleType::Fold:    return std::make_unique<Fold>();
        case ModuleType::Drive:   return std::make_unique<Drive>();
        case ModuleType::Ring:    return std::make_unique<Ring>();
        case ModuleType::Comb:    return std::make_unique<Comb>();
        case ModuleType::Formant: return std::make_unique<Formant>();
        default: return nullptr;
    }
}
} // namespace weft
