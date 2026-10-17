// SOURCE family: OSC, WAVE, NOISE, GRAIN, INPUT, PLUCK and the IN lane mixer.
// Sources add their signal to the lane input (io), so a source placed after another one sums.
#include "../Builtins.h"
#include "../Module.h"
#include "../ModuleSpec.h"
#include "../ParamMaps.h"
#include "SourceDsp.h"

#include <array>
#include <cmath>
#include <cstring>
#include <limits>

namespace weft
{
namespace
{
using namespace dsp;
using srcdsp::sin2pi;

constexpr float kOutGain = 0.5f;     // a typical source peaks around -6 dBFS
constexpr float kFmIndex = 8.0f / dsp::kTwoPi;   // FM = 1 with a 0.5 peak input -> index 4 rad

/** Mono view of the lane input at sample i. */
inline float monoIn (float* const* io, int numChannels, int i) noexcept
{
    return numChannels > 1 ? 0.5f * (io[0][i] + io[1][i]) : io[0][i];
}

/** Caches f(v) for the last value, so per-sample parameters cost nothing while they are constant. */
struct Cached
{
    float key = std::numeric_limits<float>::quiet_NaN(), value = 0.0f;
    template <typename F> float get (float v, F&& f) noexcept
    {
        if (! (v == key)) { key = v; value = f (v); }
        return value;
    }
    void invalidate() noexcept { key = std::numeric_limits<float>::quiet_NaN(); }
};

// ══ OSC ═════════════════════════════════════════════════════════════════════════════════════════

/** Waveform evaluation with per-sample shape. Discontinuities are corrected with delayed polyBLEP /
    polyBLAMP residuals (one sample of latency) so pulse width, sync and FM stay band-limited. */
struct WaveShape
{
    int wave = 0;
    float s = 0.0f;       // SAW: double-saw blend
    float pw = 0.5f;      // SQR
    float k = 0.5f;       // SIN: phase-distortion knee, TRI: peak position

    /** dt: the largest phase increment in use; TRI segments are kept at least two samples long. */
    void set (int w, float shape, float dt) noexcept
    {
        wave = w;
        shape = clamp01 (shape);
        s = shape;
        pw = 0.5f - 0.45f * shape;
        k = w == 2 ? 0.5f - 0.45f * shape : 0.5f + 0.49f * shape;
        if (w == 3) k = clampf (k, std::min (0.5f, 2.0f * dt), std::max (0.5f, 1.0f - 2.0f * dt));
    }

    float value (float p) const noexcept
    {
        switch (wave)
        {
            case 0:
            {
                const float p2 = p * 2.0f - (p >= 0.5f ? 1.0f : 0.0f);
                return (1.0f - 0.5f * s) * (2.0f * p - 1.0f) + 0.5f * s * (2.0f * p2 - 1.0f);
            }
            case 1: return (p < pw ? 1.0f : -1.0f) - (2.0f * pw - 1.0f);
            case 2:
            {
                const float q = p < k ? 0.5f * p / k : 0.5f + 0.5f * (p - k) / (1.0f - k);
                return sin2pi (q);
            }
            default:
                return p < k ? -1.0f + 2.0f * p / k : 1.0f - 2.0f * (p - k) / (1.0f - k);
        }
    }

    /** Applies residuals for every discontinuity crossed while the phase moved from q0 by inc (0 < inc < 1). */
    void correct (float q0, float inc, float& prev, float& cur, float gain) const noexcept
    {
        const float q1 = q0 + inc;
        auto cross = [&] (float loc, auto&& apply)
        {
            float l = loc <= 0.0f ? 1.0f : loc;
            if (l <= q0) l += 1.0f;
            if (l > q0 && l <= q1) apply ((q1 - l) / inc);
        };
        switch (wave)
        {
            case 0:
                cross (0.0f, [&] (float d) { srcdsp::blepStep (d, -2.0f * gain, prev, cur); });
                if (s > 0.0f) cross (0.5f, [&] (float d) { srcdsp::blepStep (d, -s * gain, prev, cur); });
                break;
            case 1:
                cross (0.0f, [&] (float d) { srcdsp::blepStep (d, 2.0f * gain, prev, cur); });
                cross (pw,   [&] (float d) { srcdsp::blepStep (d, -2.0f * gain, prev, cur); });
                break;
            case 3:
            {
                if (inc >= std::min (k, 1.0f - k)) break;   // a whole segment inside one sample (strong FM)
                const float c = (2.0f / k + 2.0f / (1.0f - k)) * inc * gain;
                cross (0.0f, [&] (float d) { srcdsp::blampCorner (d, c, prev, cur); });
                cross (k,    [&] (float d) { srcdsp::blampCorner (d, -c, prev, cur); });
                break;
            }
            default: break;
        }
    }
};

struct OscCore
{
    float phase = 0.0f;   // carrier
    float q = 0.0f;       // effective phase incl. phase modulation

    /** Advances one sample. `syncD` >= 0: hard sync happened syncD samples before this sample. */
    float tick (const WaveShape& w, float dt, float pm, float syncD, float& prev, float& cur, float gain) noexcept
    {
        float target = phase + dt + pm;
        if (syncD >= 0.0f)
        {
            // advance to the reset point, correct what was crossed, then jump back to phase 0
            const float pre = dt * (1.0f - syncD);
            const float qr = q + pre;
            w.correct (q, pre, prev, cur, gain);
            const float before = w.value (qr - std::floor (qr));
            phase = 0.0f;
            const float qa = pm - std::floor (pm);
            srcdsp::blepStep (syncD, (w.value (qa) - before) * gain, prev, cur);
            phase = dt * syncD;
            target = phase + pm;
            q = target - std::floor (target);
            return w.value (q);
        }
        phase += dt;
        phase -= std::floor (phase);
        target -= std::floor (target);
        float inc = target - q;
        if (inc < -0.5f) inc += 1.0f; else if (inc > 0.5f) inc -= 1.0f;
        if (inc > 0.0f) w.correct (q, inc, prev, cur, gain);
        q = target;
        return w.value (q);
    }
};

class Osc final : public AudioModule
{
public:
    Osc() : rng (srcdsp::nextInstanceSeed()) {}
    void prepare (double sr) override { rate = (float) sr; reset(); }
    void reset() override
    {
        a = {}; b = {};
        pending = 0.0f; lastIn = 0.0f;
        hz.invalidate(); det.invalidate();
    }
    void noteOn (const NoteState&) override
    {
        if (wave <= 1) { a.phase = a.q = rng.uniform(); b.phase = b.q = rng.uniform(); }
        else { a.phase = a.q = 0.0f; b.phase = b.q = 0.0f; }
    }

    void process (const ProcessContext& c, const ParamInputs& p, float* const* io, int nch) override
    {
        wave = std::clamp (p.mode[0], 0, 3);
        const bool sync = p.mode[1] == 1;
        const float notePitch = c.note->pitch;
        if (notePitch != lastNote) { lastNote = notePitch; hz.invalidate(); }
        WaveShape ws;
        for (int i = 0; i < c.numSamples; ++i)
        {
            const float f = hz.get (p.v[0][i], [&] (float v) { return noteToHz (notePitch + maps::semis24 (v)); });
            const float detune = clamp01 (p.v[2][i]);
            const float r = det.get (detune, [] (float v) { return std::exp2 (maps::oscDetuneCents (v) / 2400.0f); });
            ws.set (wave, p.v[1][i], std::min (0.45f, f * r / rate));
            const float fm = clamp01 (p.v[3][i]);
            const float in = monoIn (io, nch, i);

            float syncD = -1.0f;
            if (sync && lastIn < 0.0f && in >= 0.0f) syncD = in / std::max (1.0e-9f, in - lastIn);
            lastIn = in;

            const float pm = in * fm * kFmIndex;
            const float wb = std::min (1.0f, detune * 2.0f) * 0.5f;
            const float wa = 1.0f - wb;
            float cur = 0.0f;
            const float dtA = std::min (0.45f, f / (r * rate));
            const float va = a.tick (ws, dtA, pm, syncD, pending, cur, wa);
            cur += wa * va;
            if (wb > 0.0f)
            {
                const float dtB = std::min (0.45f, f * r / rate);
                const float vb = b.tick (ws, dtB, pm, syncD, pending, cur, wb);
                cur += wb * vb;
            }
            const float out = pending * kOutGain;
            pending = cur;
            const float keep = 1.0f - fm;
            for (int ch = 0; ch < nch; ++ch) io[ch][i] = io[ch][i] * keep + out;
        }
    }

private:
    float rate = 48000.0f;
    OscCore a, b;
    float pending = 0.0f, lastIn = 0.0f, lastNote = -1000.0f;
    int wave = 0;
    Cached hz, det;
    Rng rng;
};

// ══ WAVE ════════════════════════════════════════════════════════════════════════════════════════

class Wave final : public AudioModule
{
public:
    Wave()
    {
        for (int i = 0; i < 6; ++i) tables[(size_t) i] = builtinWavetable (i);
    }
    void prepare (double sr) override { rate = (float) sr; reset(); }
    void reset() override { phase = 0.0f; hz.invalidate(); }
    void noteOn (const NoteState&) override { phase = 0.0f; }

    void process (const ProcessContext& c, const ParamInputs& p, float* const* io, int nch) override
    {
        const AudioResource* t = (c.resource && c.resource->isWavetable && c.resource->numWaveFrames() > 0)
                                   ? c.resource : tables[(size_t) std::clamp (p.mode[0], 0, 5)].get();
        if (t == nullptr || t->waveLevels <= 0) return;
        const int frames = t->waveFrames;
        const int levels = t->waveLevels;
        constexpr int N = AudioResource::kWaveFrameSize;
        const float notePitch = c.note->pitch;
        if (notePitch != lastNote) { lastNote = notePitch; hz.invalidate(); }

        for (int i = 0; i < c.numSamples; ++i)
        {
            const float f = hz.get (p.v[1][i], [&] (float v) { return noteToHz (notePitch + maps::semis24 (v)); });
            const float fm = clamp01 (p.v[3][i]);
            const float in = monoIn (io, nch, i);
            const float dt = std::min (0.45f, f / rate);
            phase += dt;
            phase -= std::floor (phase);

            float q = phase + in * fm * kFmIndex;
            q -= std::floor (q);
            // WARP: Möbius phase bend, k = 2^(2w); 0.5 = none
            const float w = maps::bipolar (p.v[2][i]);
            float slope = 1.0f;
            if (w != 0.0f)
            {
                const float k = bend.get (w, [] (float x) { return std::exp2 (2.0f * x); });
                q = q / (q + k * (1.0f - q));
                slope = std::max (k, 1.0f / k);
            }

            // mip level: harmonics of level L reach (N/2 >> L) * dt * slope; keep them below Nyquist
            const float x = srcdsp::fastLog2 (std::max (1.0e-6f, (float) N * dt * slope * (1.0f + 2.0f * fm))) + 0.5f;
            int L0 = (int) std::floor (x);
            float lf = x - (float) L0;
            if (L0 < 0) { L0 = 0; lf = 0.0f; }
            if (L0 >= levels - 1) { L0 = levels - 1; lf = 0.0f; }
            const int L1 = std::min (levels - 1, L0 + 1);

            const float pos = clamp01 (p.v[0][i]) * (float) (frames - 1);
            const int f0 = std::min (frames - 1, (int) pos);
            const int f1 = std::min (frames - 1, f0 + 1);
            const float ff = pos - (float) f0;

            const float idx = q * (float) N;
            const int i0 = (int) idx & (N - 1);
            const int i1 = (i0 + 1) & (N - 1);
            const float fi = idx - std::floor (idx);
            auto rd = [&] (int fr, int lv)
            {
                const float* d = t->waveFrame (fr, lv);
                return d[i0] + (d[i1] - d[i0]) * fi;
            };
            float v = rd (f0, L0) + (rd (f1, L0) - rd (f0, L0)) * ff;
            if (lf > 0.0f)
            {
                const float v1 = rd (f0, L1) + (rd (f1, L1) - rd (f0, L1)) * ff;
                v += (v1 - v) * lf;
            }
            const float out = v * kOutGain;
            const float keep = 1.0f - fm;
            for (int ch = 0; ch < nch; ++ch) io[ch][i] = io[ch][i] * keep + out;
        }
    }

private:
    std::array<std::shared_ptr<const AudioResource>, 6> tables;
    float rate = 48000.0f, phase = 0.0f, lastNote = -1000.0f;
    Cached hz, bend;
};

// ══ NOISE ═══════════════════════════════════════════════════════════════════════════════════════

class Noise final : public AudioModule
{
public:
    Noise() : seed (srcdsp::nextInstanceSeed()) { for (int c = 0; c < 2; ++c) st[c].rng = Rng (seed + 0x9E37u * (uint32_t) (c + 1)); }
    void prepare (double sr) override
    {
        rate = (float) sr;
        darkCoef = srcdsp::onePoleCoef (180.0f, rate);
        brownLeak = 1.0f - 40.0f / rate;
        dustDecay = std::exp (-1.0f / (0.00035f * rate));
        reset();
    }
    void reset() override
    {
        for (auto& s : st) { const auto r = s.rng; s = State(); s.rng = r; }
    }

    void process (const ProcessContext& c, const ParamInputs& p, float* const* io, int nch) override
    {
        for (int i = 0; i < c.numSamples; ++i)
        {
            const float color = clamp01 (p.v[0][i]) * 3.0f;
            const int seg = std::min (2, (int) color);
            const float t = color - (float) seg;
            const float gain = lvl.get (p.v[1][i], [] (float v) { return maps::noiseLevelGain (v); });
            const float dust = clamp01 (p.v[2][i]);
            const float crush = clamp01 (p.v[3][i]);
            const float hold = crush > 0.0f ? hl.get (crush, [] (float v) { return std::pow (64.0f, v); }) : 1.0f;
            const float dustProb = dust * dust * 600.0f / rate;

            for (int ch = 0; ch < nch; ++ch)
            {
                auto& s = st[ch];
                const float w = s.rng.bipolar();
                // pink (Paul Kellet, refined)
                s.b0 = 0.99886f * s.b0 + w * 0.0555179f;
                s.b1 = 0.99332f * s.b1 + w * 0.0750759f;
                s.b2 = 0.96900f * s.b2 + w * 0.1538520f;
                s.b3 = 0.86650f * s.b3 + w * 0.3104856f;
                s.b4 = 0.55000f * s.b4 + w * 0.5329522f;
                s.b5 = -0.7616f * s.b5 - w * 0.0168980f;
                const float pink = (s.b0 + s.b1 + s.b2 + s.b3 + s.b4 + s.b5 + s.b6 + w * 0.5362f) * 0.11f;
                s.b6 = w * 0.115926f;
                s.brown = s.brown * brownLeak + w * 0.035f;
                const float brown = s.brown;
                s.dark += (brown - s.dark) * darkCoef;
                const float dark = s.dark * 0.53f;

                const float white = w * 0.36f;
                const float pinkN = pink * 1.08f;
                const float brownN = brown * 0.49f;
                float n;
                if (seg == 0) n = white + (pinkN - white) * t;
                else if (seg == 1) n = pinkN + (brownN - pinkN) * t;
                else n = brownN + (dark - brownN) * t;

                // dust: sparse crackles with a very short decay
                if (dustProb > 0.0f && s.rng.uniform() < dustProb)
                    s.crackle += (0.35f + 0.65f * s.rng.uniform()) * (s.rng.uniform() < 0.5f ? -1.0f : 1.0f) * (0.4f + 0.6f * dust);
                const float cr = s.crackle;
                s.crackle = srcdsp::flushTiny (s.crackle * dustDecay);

                float y = (n + cr) * gain;
                // crush: sample & hold
                if (hold > 1.0f)
                {
                    s.holdCount -= 1.0f;
                    if (s.holdCount <= 0.0f) { s.held = y; s.holdCount += hold; }
                    y = s.held;
                }
                io[ch][i] += srcdsp::kneeClip (y, 0.8f);
            }
        }
    }

private:
    struct State
    {
        float b0 = 0, b1 = 0, b2 = 0, b3 = 0, b4 = 0, b5 = 0, b6 = 0;
        float brown = 0, dark = 0, crackle = 0, held = 0, holdCount = 0;
        Rng rng;
    };
    State st[2];
    uint32_t seed;
    float rate = 48000.0f, darkCoef = 0.02f, brownLeak = 0.999f, dustDecay = 0.9f;
    Cached lvl, hl;
};

// ══ GRAIN ═══════════════════════════════════════════════════════════════════════════════════════

class Grain final : public AudioModule
{
public:
    Grain() : rng (srcdsp::nextInstanceSeed()) {}
    void prepare (double sr) override { rate = (float) sr; reset(); }
    void reset() override { for (auto& g : grains) g.active = false; countdown = 0.0f; }
    void noteOn (const NoteState&) override { countdown = 0.0f; }

    void process (const ProcessContext& c, const ParamInputs& p, float* const* io, int nch) override
    {
        const AudioResource* r = c.resource;
        if (r == nullptr || r->isWavetable || r->frames() < 16) return;   // passthrough
        const int frames = r->frames();
        const int rch = std::max (1, r->channels);
        const float* data = r->data.data();
        const bool key = p.mode[1] == 1;
        const int dir = std::clamp (p.mode[0], 0, 2);
        const float baseRate = (key ? std::exp2 ((c.note->pitch - 60.0f) / 12.0f) : 1.0f) * r->sampleRate / rate;

        for (int i = 0; i < c.numSamples; ++i)
        {
            countdown -= 1.0f;
            if (countdown <= 0.0f)
            {
                const float dens = maps::grainDensity (p.v[2][i]);
                const float jit = clamp01 (p.v[3][i]);
                countdown += std::max (1.0f, rate / dens * (1.0f + 0.35f * jit * rng.bipolar()));
                spawn (clamp01 (p.v[0][i]), maps::grainSizeMs (p.v[1][i]), dens, jit, baseRate, dir, frames, nch);
            }
            float l = 0.0f, rr = 0.0f;
            for (auto& g : grains)
            {
                if (! g.active) continue;
                const float t = g.age * g.invLen;
                const float wv = sin2pi (0.5f * t);
                const float win = wv * wv * g.amp;
                double pos = g.pos;
                if (pos < 0.0) pos += frames; else if (pos >= frames) pos -= frames;
                const int i0 = (int) pos;
                const int i1 = i0 + 1 < frames ? i0 + 1 : 0;
                const float fr = (float) (pos - (double) i0);
                const float s0 = data[(size_t) i0 * (size_t) rch], s1 = data[(size_t) i1 * (size_t) rch];
                const float a = s0 + (s1 - s0) * fr;
                float b = a;
                if (rch > 1)
                {
                    const float t0 = data[(size_t) i0 * (size_t) rch + 1], t1 = data[(size_t) i1 * (size_t) rch + 1];
                    b = t0 + (t1 - t0) * fr;
                }
                if (nch > 1) { l += a * win * g.panL; rr += b * win * g.panR; }
                else l += 0.5f * (a + b) * win;
                g.pos = pos + g.inc;
                g.age += 1.0f;
                if (g.age >= g.len) g.active = false;
            }
            io[0][i] += l;
            if (nch > 1) io[1][i] += rr;
        }
    }

private:
    struct G { bool active = false; double pos = 0.0, inc = 1.0; float age = 0, len = 1, invLen = 1, amp = 0, panL = 0.7f, panR = 0.7f; };

    void spawn (float pos, float sizeMs, float dens, float jit, float baseRate, int dir, int frames, int nch) noexcept
    {
        G* g = nullptr;
        for (auto& x : grains) if (! x.active) { g = &x; break; }
        if (! g) return;
        float start = pos + jit * 0.5f * rng.bipolar();
        start -= std::floor (start);
        const bool rev = dir == 1 || (dir == 2 && rng.uniform() < 0.5f);
        const float pitchJit = std::exp2 (jit * 0.15f * rng.bipolar() / 12.0f);
        g->active = true;
        g->pos = (double) start * (double) frames;
        g->inc = (double) (baseRate * pitchJit) * (rev ? -1.0 : 1.0);
        g->len = std::max (8.0f, sizeMs * 0.001f * rate);
        g->invLen = 1.0f / g->len;
        g->age = 0.0f;
        const float overlap = dens * sizeMs * 0.001f;
        g->amp = 0.9f / std::sqrt (std::max (1.0f, overlap * 0.6f));
        if (nch > 1)
        {
            const float pan = 0.5f + 0.42f * rng.bipolar();
            g->panL = std::cos (pan * 0.5f * kPi) * 1.4142f;
            g->panR = std::sin (pan * 0.5f * kPi) * 1.4142f;
        }
        else g->panL = g->panR = 1.0f;
    }

    std::array<G, 32> grains {};
    float rate = 48000.0f, countdown = 0.0f;
    Rng rng;
};

// ══ INPUT ═══════════════════════════════════════════════════════════════════════════════════════

class Input final : public AudioModule
{
public:
    void prepare (double sr) override
    {
        rate = (float) sr;
        tiltCoef = srcdsp::onePoleCoef (800.0f, rate);
        atk = 1.0f - std::exp (-1.0f / (0.001f * rate));
        rel = 1.0f - std::exp (-1.0f / (0.050f * rate));
        detRel = 1.0f - std::exp (-1.0f / (0.020f * rate));
        reset();
    }
    void reset() override { lp[0] = lp[1] = 0.0f; env = 0.0f; gate = 1.0f; }

    void process (const ProcessContext& c, const ParamInputs& p, float* const* io, int nch) override
    {
        const float* sL = c.sidechain[0];
        const float* sR = c.sidechain[1] ? c.sidechain[1] : sL;
        if (sL == nullptr) return;
        for (int i = 0; i < c.numSamples; ++i)
        {
            const float g = gainC.get (p.v[0][i], [] (float v) { return dbToGain (maps::inputGainDb (v)); });
            float l = sL[i] * g, r = sR[i] * g;
            // gate
            const float gv = clamp01 (p.v[1][i]);
            const float lvl = std::max (std::abs (l), std::abs (r));
            env = lvl > env ? lvl : env + (lvl - env) * detRel;
            float target = 1.0f;
            if (gv > 0.001f) target = gainToDb (env) > maps::inputGateDb (gv) ? 1.0f : 0.0f;
            gate += (target - gate) * (target > gate ? atk : rel);
            // tilt EQ ±6 dB around 800 Hz
            const float tilt = maps::bipolar (p.v[2][i]);
            const float gh = tiltC.get (tilt, [] (float x) { return dbToGain (6.0f * x); });
            const float glo = 1.0f / gh;
            lp[0] += (l - lp[0]) * tiltCoef;
            lp[1] += (r - lp[1]) * tiltCoef;
            l = lp[0] * glo + (l - lp[0]) * gh;
            r = lp[1] * glo + (r - lp[1]) * gh;
            // width
            const float w = clamp01 (p.v[3][i]);
            const float m = 0.5f * (l + r), s = 0.5f * (l - r) * w;
            l = srcdsp::kneeClip ((m + s) * gate, 1.0f);
            r = srcdsp::kneeClip ((m - s) * gate, 1.0f);
            if (nch > 1) { io[0][i] += l; io[1][i] += r; }
            else io[0][i] += 0.5f * (l + r);
        }
        lp[0] = srcdsp::flushTiny (lp[0]); lp[1] = srcdsp::flushTiny (lp[1]);
    }

private:
    float rate = 48000.0f, tiltCoef = 0.1f, atk = 0.1f, rel = 0.01f, detRel = 0.01f;
    float lp[2] {}, env = 0.0f, gate = 1.0f;
    Cached gainC, tiltC;
};

// ══ PLUCK ═══════════════════════════════════════════════════════════════════════════════════════

class Pluck final : public AudioModule
{
public:
    Pluck() : rng (srcdsp::nextInstanceSeed()) {}
    void prepare (double sr) override
    {
        rate = (float) sr;
        line.allocate ((int) (rate / 15.0f) + 8);
        for (int b = 0; b < 3; ++b) body[b].set (kBodyHz[b], 2.8f, rate);
        bowAtk = 1.0f - std::exp (-1.0f / (0.04f * rate));
        bowRel = 1.0f - std::exp (-1.0f / (0.15f * rate));
        reset();
    }
    void reset() override
    {
        line.clear();
        lpS = 0.0f; excLeft = 0; excLp = 0.0f; bowEnv = 0.0f; dc.reset();
        for (auto& b : body) b.reset();
        lastKey = -1.0f;
    }
    void noteOn (const NoteState& n) override { pendingVel = n.velocity; pendingExcite = true; }

    void process (const ProcessContext& c, const ParamInputs& p, float* const* io, int nch) override
    {
        const int mode = std::clamp (p.mode[0], 0, 2);
        const int n = c.numSamples;
        // pitch and loop coefficients once per sub-block (interpolating the delay for glides)
        const float tone = clamp01 (p.v[2][n - 1]);
        const float f = std::clamp (noteToHz (c.note->pitch + maps::semis24 (p.v[0][n - 1])), 20.0f, rate * 0.3f);
        const float t60 = maps::pluckDecaySec (p.v[1][n - 1]);
        const float key = f * 1000.0f + tone * 7.0f + t60;
        if (key != lastKey)
        {
            lastKey = key;
            a = 0.02f + 0.72f * (1.0f - tone) * (1.0f - tone);
            const float w = kTwoPi * f / rate;
            const float phaseDelay = std::atan2 (a * std::sin (w), 1.0f - a * std::cos (w)) / w;
            const float mag = (1.0f - a) / std::sqrt (1.0f - 2.0f * a * std::cos (w) + a * a);
            targetDelay = std::clamp (rate / f - 1.0f - phaseDelay, 2.0f, (float) line.capacity() - 4.0f);
            const float g = std::pow (10.0f, -3.0f / (t60 * f));
            fb = std::min (0.99995f, g / mag);
            if (delay <= 0.0f) delay = targetDelay;
        }
        const float d0 = delay, dStep = (targetDelay - delay) / (float) n;
        const float bodyAmt = clamp01 (p.v[3][n - 1]);
        const float excCoef = srcdsp::onePoleCoef (400.0f + 11000.0f * tone * tone, rate);
        const bool gate = c.note->gate;

        if (pendingExcite && mode == 0)
        {
            excLeft = (int) std::max (8.0f, targetDelay + 1.0f);
            excAmp = 0.55f * (0.35f + 0.65f * pendingVel);
        }
        pendingExcite = false;

        for (int i = 0; i < n; ++i)
        {
            const float d = d0 + dStep * (float) (i + 1);
            const float x = line.read (d);
            lpS = (1.0f - a) * x + a * lpS;
            const float y = lpS * fb;

            float exc = 0.0f;
            if (mode == 0)
            {
                if (excLeft > 0)
                {
                    --excLeft;
                    excLp += (rng.bipolar() - excLp) * excCoef;
                    exc = excLp * excAmp * 1.6f;
                }
            }
            else if (mode == 1)
            {
                bowEnv += ((gate ? 1.0f : 0.0f) - bowEnv) * (gate ? bowAtk : bowRel);
                excLp += (rng.bipolar() - excLp) * excCoef;
                exc = excLp * bowEnv * 0.9f * std::sqrt (std::max (0.0f, 1.0f - fb * fb));
            }
            else
            {
                exc = monoIn (io, nch, i) * std::sqrt (std::max (0.0f, 1.0f - fb * fb)) * 1.5f;
            }
            line.push (srcdsp::kneeClip (y + exc, 1.2f));

            float s = dc.process (x);
            if (bodyAmt > 0.0f)
            {
                float bsum = 0.0f;
                for (int b = 0; b < 3; ++b) bsum += body[b].process (s).bp * kBodyGain[b];
                s = s * (1.0f - 0.45f * bodyAmt) + bsum * bodyAmt * 1.4f;
            }
            const float out = srcdsp::kneeClip (s * 0.9f, 0.9f);
            for (int ch = 0; ch < nch; ++ch) io[ch][i] = (mode == 2 ? 0.0f : io[ch][i]) + out;
        }
        delay = targetDelay;
        lpS = srcdsp::flushTiny (lpS);
        excLp = srcdsp::flushTiny (excLp);
    }

private:
    static constexpr float kBodyHz[3] = { 190.0f, 440.0f, 1250.0f };
    static constexpr float kBodyGain[3] = { 1.0f, 0.8f, 0.5f };
    DelayLine line;
    Svf body[3];
    DcBlocker dc;
    float rate = 48000.0f, a = 0.3f, fb = 0.99f, delay = 0.0f, targetDelay = 100.0f, lastKey = -1.0f;
    float lpS = 0.0f, excLp = 0.0f, excAmp = 0.5f, bowEnv = 0.0f, bowAtk = 0.01f, bowRel = 0.001f, pendingVel = 0.8f;
    int excLeft = 0;
    bool pendingExcite = false;
    Rng rng;
};

// ══ IN ══════════════════════════════════════════════════════════════════════════════════════════

class In final : public AudioModule
{
public:
    void prepare (double) override {}
    void reset() override {}
    void process (const ProcessContext& c, const ParamInputs& p, float* const* io, int nch) override
    {
        if (c.laneIn == nullptr) return;
        for (int l = 0; l < kNumLanes; ++l)
        {
            if (! (c.inboundMask & (1u << l))) continue;
            const float* g = p.v[l];
            for (int ch = 0; ch < nch; ++ch)
            {
                const float* src = c.laneIn[l * 2 + (nch > 1 ? ch : 0)];
                if (src == nullptr) continue;
                for (int i = 0; i < c.numSamples; ++i) io[ch][i] += g[i] * src[i];
            }
        }
    }
};

} // namespace

std::unique_ptr<AudioModule> createSourceModule (ModuleType t)
{
    switch (t)
    {
        case ModuleType::Osc:   return std::make_unique<Osc>();
        case ModuleType::Wave:  return std::make_unique<Wave>();
        case ModuleType::Noise: return std::make_unique<Noise>();
        case ModuleType::Grain: return std::make_unique<Grain>();
        case ModuleType::Input: return std::make_unique<Input>();
        case ModuleType::Pluck: return std::make_unique<Pluck>();
        case ModuleType::In:    return std::make_unique<In>();
        default: return nullptr;
    }
}
} // namespace weft
