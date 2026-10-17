// SPACE family: DELAY, VERB, SHIFT, FREEZE, CHORUS and LOOP. Usually global stereo instances,
// but every module also runs in mono (numChannels == 1).
#include "../Module.h"
#include "../ModuleSpec.h"
#include "../ParamMaps.h"
#include "SpaceDsp.h"

#include <array>
#include <cmath>
#include <limits>

namespace weft
{
namespace
{
using namespace space;
using dsp::Rng;
using dsp::DcBlocker;

struct Cached
{
    float key = std::numeric_limits<float>::quiet_NaN(), value = 0.0f;
    template <typename F> float get (float v, F&& f) noexcept { if (! (v == key)) { key = v; value = f (v); } return value; }
    void invalidate() noexcept { key = std::numeric_limits<float>::quiet_NaN(); }
};

inline uint32_t seedFor (const void* p) noexcept { return (uint32_t) (reinterpret_cast<uintptr_t> (p) >> 4) * 2654435761u + 0x51ED27u; }

// ══ DELAY ═══════════════════════════════════════════════════════════════════════════════════════

class Delay final : public AudioModule
{
public:
    void prepare (double sr) override
    {
        rate = (float) sr;
        for (auto& r : line) r.allocate ((int) (4.2f * rate) + 64);
        slew = 1.0f - std::exp (-1.0f / (0.12f * rate));
        tapeHf = Tpt1::coef (5200.0f, rate);
        fade.prepare (rate);
        reset();
    }
    void reset() override
    {
        for (auto& r : line) r.clear();
        for (auto& f : lp) f.reset();
        for (auto& f : hp) f.reset();
        for (auto& f : tape) f.reset();
        for (auto& d : dc) d.reset();
        cur[0] = cur[1] = -1.0f;
        wow = flutter = 0.0f;
        fade.reset();
    }

    void process (const ProcessContext& c, const ParamInputs& p, float* const* io, int nch) override
    {
        const int type = clampi (p.mode[0], 0, 2);
        const bool sync = p.mode[1] == 1;
        const float spb = c.transport ? c.transport->samplesPerBeat : rate * 0.5f;
        const float maxD = 4.0f * rate;
        for (int i = 0; i < c.numSamples; ++i)
        {
            const float tv = clamp01 (p.v[0][i]);
            float target = sync ? dsp::kDivisions[dsp::divisionIndex (tv)].beats * spb : maps::delayFreeMs (tv) * 0.001f * rate;
            target = clampf (target, 2.0f, maxD);
            const float fb = maps::delayFeedback (p.v[1][i]);
            const float tone = clamp01 (p.v[2][i]);
            const MixGains mg = mixLaw (p.v[3][i]);
            const float lpG = tone < 0.5f ? lpC.get (tone, [this] (float t) { return Tpt1::coef (800.0f * std::pow (25.0f, t * 2.0f), rate); }) : 1.0f;
            const float hpG = tone > 0.5f ? hpC.get (tone, [this] (float t) { return Tpt1::coef (20.0f * std::pow (15.0f, (t - 0.5f) * 2.0f), rate); }) : 0.0f;
            if (fade.tick (type)) { for (auto& r : line) r.clear(); }
            const float g = fade.gain;

            float mod = 0.0f;
            if (type == 2)
            {
                wow += 0.45f / rate; wow -= std::floor (wow);
                flutter += 6.3f / rate; flutter -= std::floor (flutter);
                mod = 1.0f + 0.0035f * sin2pi (wow) + 0.0006f * sin2pi (flutter);
            }

            float rd[2] = { 0.0f, 0.0f };
            for (int ch = 0; ch < nch; ++ch)
            {
                float t = target * (type == 1 && ch == 1 ? 1.03f : 1.0f) * (type == 2 ? mod : 1.0f);
                t = clampf (t, 2.0f, maxD);
                if (cur[ch] < 0.0f) cur[ch] = t;
                cur[ch] += (t - cur[ch]) * slew;
                float y = line[ch].cubic (cur[ch]);
                if (lpG < 1.0f) y = lp[ch].lp (y, lpG);
                if (hpG > 0.0f) y = y - hp[ch].lp (y, hpG);
                if (type == 2) y = tape[ch].lp (y, tapeHf);
                rd[ch] = y;
            }
            const float inL = io[0][i], inR = nch > 1 ? io[1][i] : inL;
            float wL, wR;
            if (nch > 1 && type == 0) { wL = 0.5f * (inL + inR) + fb * rd[1]; wR = fb * rd[0]; }
            else { wL = inL + fb * rd[0]; wR = inR + fb * rd[nch > 1 ? 1 : 0]; }
            wL = dc[0].process (wL); if (nch > 1) wR = dc[1].process (wR);
            if (type == 2) { wL = tapeSat (wL, 1.6f); wR = tapeSat (wR, 1.6f); }
            line[0].write (kneeSat (wL, 0.9f, 1.6f));
            if (nch > 1) line[1].write (kneeSat (wR, 0.9f, 1.6f));
            io[0][i] = inL * mg.dry + rd[0] * mg.wet * g;
            if (nch > 1) io[1][i] = inR * mg.dry + rd[1] * mg.wet * g;
        }
        for (int ch = 0; ch < 2; ++ch) { lp[ch].flush(); hp[ch].flush(); tape[ch].flush(); }
    }

private:
    Ring line[2];
    Tpt1 lp[2], hp[2], tape[2];
    DcBlocker dc[2];
    ModeFade fade;
    float rate = 48000.0f, slew = 0.001f, tapeHf = 0.3f, cur[2] { -1.0f, -1.0f }, wow = 0.0f, flutter = 0.0f;
    Cached lpC, hpC;
};

// ══ VERB ════════════════════════════════════════════════════════════════════════════════════════

class Verb final : public AudioModule
{
public:
    Verb() : rng (seedFor (this)) {}
    void prepare (double sr) override
    {
        rate = (float) sr;
        const float k = rate / 48000.0f;
        for (int i = 0; i < 8; ++i) lines[i].allocate ((int) (kLen[i] * 1.35f * k) + 64);
        for (int i = 0; i < 4; ++i) for (int ch = 0; ch < 2; ++ch) { diff[ch][i].allocate ((int) (kDiff[i] * 1.2f * k) + 4); diff[ch][i].setDelay ((int) (kDiff[i] * k * (ch ? 1.07f : 1.0f))); }
        pre.allocate ((int) (0.26f * rate) + 8);
        fade.prepare (rate);
        reset();
    }
    void reset() override
    {
        for (auto& l : lines) l.clear();
        for (auto& d : diff) for (auto& a : d) a.clear();
        pre.clear();
        for (auto& f : damp) f.reset();
        for (int i = 0; i < 8; ++i) lfo[i] = (float) i / 8.0f;
        hold = 0.0f;
        fade.reset();
        lastKey = -1.0f;
    }

    void process (const ProcessContext& c, const ParamInputs& p, float* const* io, int nch) override
    {
        const int type = clampi (p.mode[0], 0, 2);
        const int n = c.numSamples;
        const float k = rate / 48000.0f;
        const float scale = type == 0 ? 0.42f : (type == 1 ? 1.0f : 1.3f);
        const float sizeV = clamp01 (p.v[0][n - 1]);
        const float dampV = clamp01 (p.v[1][n - 1]);
        const bool inf = maps::verbInfinite (sizeV);
        const float key = sizeV * 13.0f + dampV + (float) type * 101.0f;
        if (key != lastKey)
        {
            lastKey = key;
            const float t60 = maps::verbDecaySec (sizeV);
            for (int i = 0; i < 8; ++i)
            {
                len[i] = kLen[i] * scale * k;
                gain[i] = inf ? 1.0f : std::pow (10.0f, -3.0f * len[i] / (t60 * rate));
            }
            dampG = Tpt1::coef (18000.0f * std::pow (1.0f / 12.0f, dampV), rate);
            diffG = type == 2 ? 0.72f : (type == 0 ? 0.55f : 0.62f);
            modDepth = (type == 2 ? 22.0f : 5.0f) * k;
        }
        const float holdTarget = inf ? 1.0f : 0.0f;
        const float holdStep = 1.0f / (0.05f * rate);
        const float lfoInc[8] = { 0.11f / rate, 0.17f / rate, 0.23f / rate, 0.29f / rate, 0.37f / rate, 0.41f / rate, 0.53f / rate, 0.61f / rate };
        constexpr float outGain = 0.42f;

        for (int i = 0; i < n; ++i)
        {
            const float preD = maps::verbPreMs (p.v[2][i]) * 0.001f * rate;
            const MixGains mg = mixLaw (p.v[3][i]);
            if (fade.tick (type)) { for (auto& l : lines) l.clear(); for (auto& d : diff) for (auto& a : d) a.clear(); }
            hold += clampf (holdTarget - hold, -holdStep, holdStep);

            const float inL = io[0][i], inR = nch > 1 ? io[1][i] : inL;
            const float inM = 0.5f * (inL + inR), inS = 0.5f * (inL - inR);
            // pre-delay on the mid signal, side is kept for width
            pre.write (inM);
            float xL = (preD >= 2.0f ? pre.cubic (preD) : inM) + inS, xR = (preD >= 2.0f ? pre.cubic (preD) : inM) - inS;
            const float inGain = 1.0f - hold;
            xL *= inGain; xR *= inGain;
            for (int d = 0; d < 4; ++d) { xL = diff[0][d].process (xL, diffG); xR = diff[1][d].process (xR, diffG); }

            float v[8];
            for (int j = 0; j < 8; ++j)
            {
                lfo[j] += lfoInc[j]; if (lfo[j] >= 1.0f) lfo[j] -= 1.0f;
                const float d = len[j] + modDepth * (1.0f + sin2pi (lfo[j]));
                float y = lines[j].linear (d);
                if (hold < 1.0f) y = damp[j].lp (y, dampG) * (1.0f - hold) + y * hold;
                v[j] = y * gain[j];
            }
            const float oL = v[0] - v[2] + v[4] - v[6] + 0.5f * (v[1] - v[5]);
            const float oR = v[1] - v[3] + v[5] - v[7] + 0.5f * (v[2] - v[6]);
            hadamard8 (v);
            for (int j = 0; j < 8; ++j)
            {
                const float in = (j & 1) ? xR : xL;
                lines[j].write (kneeSat (v[j] + in * 0.5f, 1.5f, 2.5f));
            }
            const float g = fade.gain * outGain;
            if (nch > 1)
            {
                io[0][i] = inL * mg.dry + oL * g * mg.wet;
                io[1][i] = inR * mg.dry + oR * g * mg.wet;
            }
            else io[0][i] = inL * mg.dry + 0.5f * (oL + oR) * g * mg.wet;
        }
        for (auto& f : damp) f.flush();
    }

private:
    static constexpr float kLen[8] = { 1153, 1327, 1559, 1801, 2063, 2357, 2671, 3011 };
    static constexpr float kDiff[4] = { 142, 107, 379, 277 };
    Ring lines[8];
    Allpass diff[2][4];
    Ring pre;
    Tpt1 damp[8];
    ModeFade fade;
    float len[8] {}, gain[8] {}, lfo[8] {};
    float rate = 48000.0f, dampG = 0.5f, diffG = 0.6f, modDepth = 5.0f, hold = 0.0f, lastKey = -1.0f;
    Rng rng;
};

// ══ SHIFT ═══════════════════════════════════════════════════════════════════════════════════════

class Shift final : public AudioModule
{
public:
    Shift() : rng (seedFor (this)) {}
    void prepare (double sr) override
    {
        rate = (float) sr;
        for (auto& r : buf) r.allocate ((int) (0.5f * rate) + 64);
        for (auto& ch : diff) for (int i = 0; i < 3; ++i) { ch[i].allocate ((int) (kDiff[i] * rate / 48000.0f) + 4); ch[i].setDelay ((int) (kDiff[i] * rate / 48000.0f)); }
        fade.prepare (rate);
        reset();
    }
    void reset() override
    {
        for (auto& r : buf) r.clear();
        for (auto& ch : diff) for (auto& a : ch) a.clear();
        for (int h = 0; h < 4; ++h) { phase[h] = (float) h / 4.0f; extra[h] = 0.0f; }
        fbState[0] = fbState[1] = 0.0f;
        fade.reset();
    }

    void process (const ProcessContext& c, const ParamInputs& p, float* const* io, int nch) override
    {
        const int type = clampi (p.mode[0], 0, 1);
        const bool quant = p.mode[1] == 1;
        const int heads = type == 0 ? 2 : 4;
        const float W = (type == 0 ? 0.05f : 0.09f) * rate;
        const float minD = 0.003f * rate;
        const float norm = type == 0 ? 1.0f : 0.5f;
        for (int i = 0; i < c.numSamples; ++i)
        {
            float st = maps::semis24 (p.v[0][i]);
            if (quant) st = std::round (st);
            const float r = ratioC.get (st, [] (float s) { return std::exp2 (s / 12.0f); });
            const float blur = clamp01 (p.v[1][i]) * (type == 1 ? 1.0f : 0.6f);
            const float fb = clamp01 (p.v[2][i]) * 0.85f;
            const MixGains mg = mixLaw (p.v[3][i]);
            if (fade.tick (type)) for (int h = 0; h < 4; ++h) { phase[h] = (float) h / (float) heads; extra[h] = 0.0f; }
            const float inc = std::abs (1.0f - r) / W;
            for (int h = 0; h < heads; ++h)
            {
                phase[h] += inc;
                if (phase[h] >= 1.0f) { phase[h] -= 1.0f; extra[h] = blur * W * 0.8f * rng.uniform(); }
            }
            for (int ch = 0; ch < nch; ++ch)
            {
                const float x = io[ch][i];
                buf[ch].write (x + fbState[ch]);
                float y = 0.0f;
                for (int h = 0; h < heads; ++h)
                {
                    const float ph = phase[h];
                    const float d = minD + extra[h] + (r > 1.0f ? W * (1.0f - ph) : W * ph);
                    y += buf[ch].cubic (d + 1.0f) * hann (ph);
                }
                y *= norm;
                if (blur > 0.0f) for (auto& a : diff[ch]) y = a.process (y, 0.55f * blur);
                fbState[ch] = kneeSat (y * fb, 0.8f, 1.2f);
                io[ch][i] = x * mg.dry + y * mg.wet * fade.gain;
            }
        }
    }

private:
    static constexpr float kDiff[3] = { 113, 337, 241 };
    Ring buf[2];
    Allpass diff[2][3];
    ModeFade fade;
    float phase[4] {}, extra[4] {}, fbState[2] {};
    float rate = 48000.0f;
    Cached ratioC;
    Rng rng;
};

// ══ FREEZE ══════════════════════════════════════════════════════════════════════════════════════

class Freeze final : public AudioModule
{
public:
    Freeze() : rng (seedFor (this)) {}
    void prepare (double sr) override
    {
        rate = (float) sr;
        for (auto& r : rec) r.allocate ((int) (2.6f * rate) + 64);
        for (auto& f : frozen) f.assign ((size_t) (2.05f * rate) + 64, 0.0f);
        reset();
    }
    void reset() override
    {
        for (auto& r : rec) r.clear();
        h = 0.0f; frozenOn = false; segLen = 0;
        for (int g = 0; g < 4; ++g) { gPhase[g] = (float) g / 4.0f; gStart[g] = 0.0f; }
    }

    void process (const ProcessContext& c, const ParamInputs& p, float* const* io, int nch) override
    {
        for (int i = 0; i < c.numSamples; ++i)
        {
            const bool want = p.v[0][i] >= 0.5f;
            if (want && ! frozenOn) capture (maps::freezeSizeMs (p.v[2][i]) * 0.001f * rate, nch);
            frozenOn = want;
            const float target = want ? 1.0f : 0.0f;
            if (h != target)
            {
                const float step = 1.0f / std::max (1.0f, maps::freezeFadeSec (p.v[1][i]) * rate);
                h = target > h ? std::min (target, h + step) : std::max (target, h - step);
            }
            for (int ch = 0; ch < nch; ++ch) rec[ch].write (io[ch][i]);
            if (h <= 0.0f) continue;   // bit-identical passthrough

            // four overlapping Hann grains at random positions inside the captured segment
            const float gl = grainLen;
            float out[2] = { 0.0f, 0.0f };
            for (int g = 0; g < 4; ++g)
            {
                gPhase[g] += 1.0f / gl;
                if (gPhase[g] >= 1.0f) { gPhase[g] -= 1.0f; gStart[g] = rng.uniform() * std::max (0.0f, (float) segLen - gl); }
                const float w = hann (gPhase[g]);
                float pos = gStart[g] + gPhase[g] * gl;
                if (pos >= (float) segLen) pos -= (float) segLen;
                const int i0 = (int) pos;
                const int i1 = i0 + 1 < segLen ? i0 + 1 : 0;
                const float fr = pos - (float) i0;
                for (int ch = 0; ch < nch; ++ch)
                {
                    const float* f = frozen[ch].data();
                    out[ch] += (f[i0] + (f[i1] - f[i0]) * fr) * w;
                }
            }
            const float mix = clamp01 (p.v[3][i]) * h;
            for (int ch = 0; ch < nch; ++ch) io[ch][i] = io[ch][i] * (1.0f - mix) + out[ch] * 0.5f * mix;
        }
    }

private:
    void capture (float len, int nch) noexcept
    {
        segLen = clampi ((int) len, 64, (int) frozen[0].size() - 2);
        for (int ch = 0; ch < 2; ++ch)
        {
            const Ring& r = rec[ch < nch ? ch : 0];
            float* f = frozen[ch].data();
            // oldest first; a short fade at both ends keeps grain wrap-around seamless
            for (int k = 0; k < segLen; ++k) f[k] = r.at ((uint32_t) (segLen - k));
            const int edge = std::min (segLen / 4, (int) (0.005f * rate));
            for (int k = 0; k < edge; ++k)
            {
                const float w = (float) k / (float) edge;
                f[k] *= w; f[segLen - 1 - k] *= w;
            }
        }
        grainLen = clampf ((float) segLen * 0.5f, 0.02f * rate, (float) segLen);
        for (int g = 0; g < 4; ++g) { gPhase[g] = (float) g / 4.0f; gStart[g] = rng.uniform() * std::max (0.0f, (float) segLen - grainLen); }
    }

    Ring rec[2];
    std::vector<float> frozen[2];
    float rate = 48000.0f, h = 0.0f, grainLen = 1000.0f;
    bool frozenOn = false;
    int segLen = 0;
    float gPhase[4] {}, gStart[4] {};
    Rng rng;
};

// ══ CHORUS ══════════════════════════════════════════════════════════════════════════════════════

class Chorus final : public AudioModule
{
public:
    void prepare (double sr) override
    {
        rate = (float) sr;
        for (auto& r : line) r.allocate ((int) (0.05f * rate) + 64);
        fade.prepare (rate);
        reset();
    }
    void reset() override { for (auto& r : line) r.clear(); lfo = 0.0f; vib = 0.0f; fbS[0] = fbS[1] = 0.0f; fade.reset(); }

    void process (const ProcessContext& c, const ParamInputs& p, float* const* io, int nch) override
    {
        const int type = clampi (p.mode[0], 0, 2);
        const float ms = rate * 0.001f;
        for (int i = 0; i < c.numSamples; ++i)
        {
            const float hz = rateC.get (p.v[0][i], [] (float v) { return maps::chorusRateHz (v); });
            const float depth = clamp01 (p.v[1][i]);
            const float fbv = clamp01 (p.v[2][i]);
            const MixGains mg = mixLaw (p.v[3][i]);
            if (fade.tick (type)) { for (auto& r : line) r.clear(); fbS[0] = fbS[1] = 0.0f; }
            lfo += hz / rate; lfo -= std::floor (lfo);
            vib += 6.1f / rate; vib -= std::floor (vib);
            for (int ch = 0; ch < nch; ++ch)
            {
                const float x = io[ch][i];
                const float off = ch == 1 ? 0.5f : 0.0f;
                float y = 0.0f, fbGain;
                if (type == 0)
                {
                    for (int v = 0; v < 3; ++v)
                    {
                        const float d = (7.0f + 4.0f * (float) v + depth * 5.0f * (1.0f + sin2pi (lfo + off + (float) v / 3.0f))) * ms;
                        y += line[ch].cubic (d);
                    }
                    y *= 0.4f;
                    fbGain = fbv * 0.6f;
                }
                else if (type == 1)
                {
                    const float d = (0.5f + 4.5f * depth * 0.5f * (1.0f + sin2pi (lfo + off))) * ms;
                    y = line[ch].cubic (std::max (2.0f, d));
                    fbGain = fbv * 0.95f;
                }
                else
                {
                    for (int v = 0; v < 3; ++v)
                    {
                        const float ph = (float) v / 3.0f;
                        const float d = (5.0f + depth * 3.0f * (1.0f + sin2pi (lfo + ph + off)) + 0.25f * depth * sin2pi (vib + ph)) * ms;
                        y += line[ch].cubic (d);
                    }
                    y *= 0.42f;
                    fbGain = fbv * 0.5f;
                }
                line[ch].write (kneeSat (x + fbS[ch], 1.2f, 2.0f));
                fbS[ch] = y * fbGain;
                io[ch][i] = x * mg.dry + y * mg.wet * fade.gain;
            }
        }
    }

private:
    Ring line[2];
    ModeFade fade;
    float rate = 48000.0f, lfo = 0.0f, vib = 0.0f, fbS[2] {};
    Cached rateC;
};

// ══ LOOP ════════════════════════════════════════════════════════════════════════════════════════

class Loop final : public AudioModule
{
public:
    void prepare (double sr) override
    {
        rate = (float) sr;
        cap = (int) (8.0f * rate) + 8;
        for (auto& b : buf) b.assign ((size_t) cap, 0.0f);
        xfLen = 0.006f * rate;
        reset();
    }
    void reset() override
    {
        for (auto& b : buf) std::fill (b.begin(), b.end(), 0.0f);
        w = 0; rpos = 0.0; len = (int) rate; prevLen = len; xf = 0.0f;
    }

    void process (const ProcessContext& c, const ParamInputs& p, float* const* io, int nch) override
    {
        const bool recording = p.mode[0] == 0;
        const bool sync = p.mode[1] == 1;
        const float spb = c.transport ? c.transport->samplesPerBeat : rate * 0.5f;
        for (int i = 0; i < c.numSamples; ++i)
        {
            const float speed = maps::loopSpeed (p.v[0][i]);
            const float lv = clamp01 (p.v[1][i]);
            int L = (int) (sync ? maps::kSyncRates[maps::syncRateIndex (lv)].beats * spb : maps::loopLengthSec (lv) * rate);
            L = clampi (L, 64, cap - 2);
            if (L != len) { prevLen = len; len = L; xf = 1.0f; if (w >= len) w %= len; }
            const float feed = clamp01 (p.v[2][i]);
            const MixGains mg = mixLaw (p.v[3][i]);

            // read head with linear interpolation, crossfading across loop-length changes
            double pos = std::fmod (rpos, (double) len);
            if (pos < 0.0) pos += len;
            const int i0 = (int) pos;
            const int i1 = i0 + 1 < len ? i0 + 1 : 0;
            const float fr = (float) (pos - (double) i0);
            float y[2];
            for (int ch = 0; ch < nch; ++ch) { const float* b = buf[ch].data(); y[ch] = b[i0] + (b[i1] - b[i0]) * fr; }
            // short fade around the write head so the overdub seam does not click
            const float dist = std::abs ((float) pos - (float) w);
            const float seam = std::min (dist, (float) len - dist);
            const float seamGain = (recording && std::abs (speed - 1.0f) > 0.01f && seam < xfLen) ? 0.5f + 0.5f * seam / xfLen : 1.0f;
            const float g = seamGain * (1.0f - xf);
            if (xf > 0.0f) xf = std::max (0.0f, xf - 1.0f / xfLen);

            for (int ch = 0; ch < nch; ++ch)
            {
                const float x = io[ch][i];
                if (recording)
                {
                    float& b = buf[ch][(size_t) w];
                    b = kneeSat (b * feed + x, 1.0f, 1.5f);
                }
                io[ch][i] = x * mg.dry + y[ch] * g * mg.wet;
            }
            if (++w >= len) w = 0;
            rpos += speed;
            if (rpos >= len) rpos -= len; else if (rpos < 0.0) rpos += len;
        }
    }

private:
    std::vector<float> buf[2];
    int cap = 0, w = 0, len = 48000, prevLen = 48000;
    double rpos = 0.0;
    float rate = 48000.0f, xf = 0.0f, xfLen = 256.0f;
};

} // namespace

std::unique_ptr<AudioModule> createSpaceModule (ModuleType t)
{
    switch (t)
    {
        case ModuleType::Delay:  return std::make_unique<Delay>();
        case ModuleType::Verb:   return std::make_unique<Verb>();
        case ModuleType::Shift:  return std::make_unique<Shift>();
        case ModuleType::Freeze: return std::make_unique<Freeze>();
        case ModuleType::Chorus: return std::make_unique<Chorus>();
        case ModuleType::Loop:   return std::make_unique<Loop>();
        default: return nullptr;
    }
}
} // namespace weft
