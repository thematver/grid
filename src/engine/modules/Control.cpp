// CONTROL family — modulators: LFO, ENV, RAND, SEQ, FOLLOW, DRIFT, LOGIC, MATH, CLOCK, PHYSICS.
//
// Every process() advances ctx.numSamples samples (parameters are per-sample arrays) and returns the
// value at the END of the sub-block, in [0,1]. Rate / time coefficients are evaluated per 8-sample
// chunk and cached, so constant parameters cost nothing and modulated ones stay accurate.
// Note events are replayed at their sample offsets (ctx.note->trigger / release when inside the
// sub-block). Nothing here allocates, locks or throws.
#include "ControlDsp.h"
#include "../ModuleSpec.h"

namespace weft
{
namespace
{
using namespace ctl;
using dsp::clamp01;
using dsp::kPi;
using dsp::kTwoPi;

const float kZeroAux[kAuxSize] {};
inline const float* auxOf (const ControlContext& c) noexcept { return c.aux ? c.aux : kZeroAux; }

struct ControlBase : ControlModule
{
    float sr = 48000.0f;

    void prepare (double sampleRate) override
    {
        sr = (sampleRate >= 1000.0 && sampleRate < 1.0e7) ? (float) sampleRate : 48000.0f;
        rateChanged();
    }
    /** Invalidate sample-rate dependent caches. */
    virtual void rateChanged() {}

    /** The context's rate wins over the prepared one (they only differ if prepare() was skipped). */
    void followRate (const ControlContext& c) noexcept
    {
        if (c.sampleRate >= 1000.0f && c.sampleRate < 1.0e7f && c.sampleRate != sr) { sr = c.sampleRate; rateChanged(); }
    }
};

// ── LFO ─────────────────────────────────────────────────────────────────────────────────────────

/** Unipolar LFO shapes. x = phase [0,1), s = SHAPE [0,1]. */
float lfoWave (int wave, float x, float s) noexcept
{
    switch (wave)
    {
        case 0:   // TRI: SHAPE moves the peak (0.5 symmetric); the ends become falling / rising ramps
            if (s <= 1.0e-4f) return 1.0f - x;
            if (s >= 1.0f - 1.0e-4f) return x;
            return x < s ? x / s : (1.0f - x) / (1.0f - s);
        case 1:   // SIN: below 0.5 curvature (rounds towards a soft square), above 0.5 asymmetry (fast rise)
        {
            float u = x;
            if (s > 0.5f)
            {
                const float p = 0.5f - 0.9f * (s - 0.5f);   // rising portion 0.5 .. 0.05 of the cycle
                u = x < p ? 0.5f * x / p : 0.5f + 0.5f * (x - p) / (1.0f - p);
            }
            float y = -std::cos (kTwoPi * u);
            if (s < 0.5f)
            {
                const float k = (0.5f - s) * 2.0f;
                y += k * (std::tanh (4.0f * y) * (1.0f / 0.99932930f) - y);   // tanh(4) normalisation
            }
            return 0.5f + 0.5f * y;
        }
        case 2:   // SAW: rising ramp, SHAPE bends it from x^4 through linear to x^0.25
            return x <= 0.0f ? 0.0f : std::pow (x, std::exp2 (2.0f - 4.0f * s));
        default:  // SQR: SHAPE is the pulse width, 5 .. 95 %
            return x < 0.05f + 0.9f * s ? 1.0f : 0.0f;
    }
}

struct Lfo final : ControlBase
{
    double ph = 0.0;          // running phase without the PHASE offset
    float shown = 0.0f;       // phase incl. offset (what the output is at)
    float out = 0.0f;
    double fadeT = 1.0e9;     // seconds since the fade-in started (huge = fully faded in)
    bool held = false;
    MapCache rate;
    NoteEvents ev;

    void rateChanged() override { rate.invalidate(); }
    void reset() override { ph = 0.0; shown = 0.0f; out = 0.0f; fadeT = 1.0e9; held = false; ev.clear(); rate.invalidate(); }
    void noteOn (const NoteState&) override { ev.noteOn(); }
    void noteOff() override { ev.noteOff(); }
    float phase() const override { return shown; }

    float process (const ControlContext& c, const ParamInputs& p) override
    {
        followRate (c);
        const int n = c.numSamples;
        if (n <= 0) return out;
        const int wave = clampi (p.mode[0], 0, 3);
        const int timing = clampi (p.mode[1], 0, 2);
        const auto s = ev.take (c);

        int fadeFrom = -1, keyAt = -1;
        for (int e = 0; e < s.count; ++e)
        {
            if (s.e[e].isOn)
            {
                if (! held) fadeFrom = s.e[e].at;   // legato notes keep the fade going
                held = true;
                keyAt = s.e[e].at;
            }
            else held = false;
        }

        auto hz = [] (float v) { return maps::lfoRateHz (v); };
        const Transport& t = transportOf (c);
        if (timing == 1)
        {
            if (t.playing)
            {
                const double x = endPpq (t, n) / (double) maps::syncRateBeats (safe01 (p.v[0][n - 1]));
                ph = x - std::floor (x);
            }
            else ph += syncCycles (p.v[0], 0, n, samplesPerBeat (t));
        }
        else if (timing == 2 && keyAt >= 0)
            ph = freeCycles (p.v[0], keyAt, n, sr, rate, hz);   // retrigger: restart at PHASE
        else
            ph += freeCycles (p.v[0], 0, n, sr, rate, hz);
        ph -= std::floor (ph);
        if (! (ph >= 0.0 && ph < 1.0)) ph = 0.0;

        double x = ph + (double) safe01 (p.v[2][n - 1]);
        x -= std::floor (x);
        shown = (x >= 0.0 && x < 1.0) ? (float) x : 0.0f;
        if (shown >= 1.0f) shown = 0.0f;

        const double dt = 1.0 / (double) sr;
        fadeT = fadeFrom >= 0 ? (double) (n - fadeFrom) * dt : std::min (fadeT + (double) n * dt, 1.0e9);
        const double fadeSec = 10.0 * (double) safe01 (p.v[3][n - 1]);
        const float fade = fadeSec > 1.0e-4 ? (float) std::min (1.0, fadeT / fadeSec) : 1.0f;

        out = clamp01 (lfoWave (wave, shown, safe01 (p.v[1][n - 1])) * fade);
        return out;
    }
};

// ── ENV ─────────────────────────────────────────────────────────────────────────────────────────
// Stage times: EXP — attack reaches 90 % at A, decay / release cover 90 % of their swing at D / R
// (exponential, -20 dB per stage time). LIN — each stage completes its full swing in its time.

struct Env final : ControlBase
{
    enum Stage : uint8_t { Idle, Attack, Decay, Sustain, Release };
    enum Type : int { kADSR = 0, kAD = 1, kLoop = 2 };
    static constexpr float kOver = 1.5f;              // EXP attack charges towards 1.5 (concave, analog style)
    static constexpr float kAttackK = 0.91629073f;    // ln (1.5 / (1.5 - 0.9)): 90 % after A
    static constexpr float kLn10 = 2.30258509f;

    struct Coef { float key = -1.0f; int curve = -1; float c = 0.0f; };

    Stage stage = Idle;
    float level = 0.0f, relStart = 1.0f, sus = 0.0f;
    bool gate = false;
    int type = kADSR, curve = 0;
    Coef ca, cd, cr;
    NoteEvents ev;

    void rateChanged() override { ca = cd = cr = Coef(); }
    void reset() override { stage = Idle; level = 0.0f; relStart = 1.0f; sus = 0.0f; gate = false; ev.clear(); }
    void noteOn (const NoteState&) override { ev.noteOn(); }
    void noteOff() override { ev.noteOff(); }

    void ensure (Coef& co, float v, bool attack) noexcept
    {
        if (v == co.key && curve == co.curve) return;
        co.key = v; co.curve = curve;
        const float t = attack ? maps::envAttackSec (v) : maps::envTimeSec (v);
        const float samples = t * sr;
        if (curve == 1) co.c = 1.0f / samples;
        else co.c = attack ? 1.0f - std::exp (-kAttackK / samples) : std::exp (-kLn10 / samples);
    }

    void refresh (const ParamInputs& p, int i) noexcept
    {
        switch (stage)
        {
            case Attack:  ensure (ca, safe01 (p.v[0][i]), true); break;
            case Decay:   ensure (cd, safe01 (p.v[1][i]), false); break;
            case Release: ensure (cr, safe01 (p.v[3][i]), false); break;
            default: break;
        }
    }

    void enter (Stage st, const ParamInputs& p, int i) noexcept
    {
        stage = st;
        if (st == Release) relStart = std::max (level, 1.0e-4f);
        refresh (p, i);
    }

    float process (const ControlContext& c, const ParamInputs& p) override
    {
        followRate (c);
        const int n = c.numSamples;
        if (n <= 0) return level;
        type = clampi (p.mode[0], 0, 2);
        curve = clampi (p.mode[1], 0, 1);
        const bool lin = curve == 1;
        const auto s = ev.take (c);
        int next = 0;

        for (int i = 0; i < n; ++i)
        {
            while (next < s.count && s.e[next].at == i)
            {
                if (s.e[next].isOn) { gate = true; enter (Attack, p, i); }   // retrigger from the current level
                else
                {
                    gate = false;
                    if (type != kAD && stage != Idle) enter (Release, p, i);
                }
                ++next;
            }
            sus = safe01 (p.v[2][i]);
            if ((i & (kChunk - 1)) == 0) refresh (p, i);
            if (! gate && type != kAD && (stage == Attack || stage == Decay || stage == Sustain)) enter (Release, p, i);

            switch (stage)
            {
                case Idle: break;
                case Attack:
                    level = lin ? level + ca.c : level + (kOver - level) * ca.c;
                    if (level >= 1.0f) { level = 1.0f; enter (Decay, p, i); }
                    break;
                case Decay:
                {
                    const float target = type == kAD ? 0.0f : sus;
                    bool done;
                    if (lin)
                    {
                        level -= (1.0f - target) * cd.c;
                        done = level <= target;
                        if (done) level = target;
                    }
                    else
                    {
                        level = target + (level - target) * cd.c;
                        const float rem = level - target;
                        done = type == kLoop ? rem <= 0.1f * (1.0f - target) + 1.0e-6f : std::abs (rem) < 1.0e-4f;
                    }
                    if (done)
                    {
                        if (type == kAD) { level = 0.0f; stage = Idle; }
                        else if (type == kLoop) enter (Attack, p, i);
                        else { level = target; stage = Sustain; }
                    }
                    break;
                }
                case Sustain:
                    if (type == kAD) enter (Decay, p, i);
                    else if (type == kLoop) enter (Attack, p, i);
                    else level = sus;
                    break;
                case Release:
                    if (lin) { level -= relStart * cr.c; if (level <= 0.0f) { level = 0.0f; stage = Idle; } }
                    else     { level *= cr.c;            if (level < 1.0e-4f) { level = 0.0f; stage = Idle; } }
                    break;
            }
        }
        if (! std::isfinite (level)) { level = 0.0f; stage = Idle; }
        return clamp01 (level);
    }

    /** ADSR: A 0–.25, D .25–.5, S .5, R .75–1 · AD: A 0–.5, D .5–1 · LOOP: A 0–⅓, D ⅓–⅔, R ⅔–1. */
    float phase() const override
    {
        if (stage == Idle) return -1.0f;
        const float target = type == kAD ? 0.0f : sus;
        const float decProg = (1.0f - target) > 1.0e-6f ? clamp01 ((1.0f - level) / (1.0f - target)) : 1.0f;
        const float relProg = clamp01 (1.0f - level / relStart);
        float x = 0.0f;
        if (type == kAD)
            x = stage == Attack ? 0.5f * level : (stage == Release ? 0.5f + 0.5f * relProg : 0.5f + 0.5f * decProg);
        else if (type == kLoop)
            x = stage == Attack ? level / 3.0f : stage == Decay ? (1.0f + decProg) / 3.0f
              : stage == Release ? (2.0f + relProg) / 3.0f : 1.0f / 3.0f;
        else
            x = stage == Attack ? 0.25f * level : stage == Decay ? 0.25f + 0.25f * decProg
              : stage == Release ? 0.75f + 0.25f * relProg : 0.5f;
        return std::min (std::max (x, 0.0f), 0.9999f);
    }
};

// ── RAND ────────────────────────────────────────────────────────────────────────────────────────

struct Rand final : ControlBase
{
    const uint32_t seed = nextSeed();
    dsp::Rng rng { seed };
    StepClock clk;
    float from = 0.5f, target = 0.5f, out = 0.5f;
    MapCache rate;

    void rateChanged() override { rate.invalidate(); }
    void reset() override { rng = dsp::Rng (seed); clk.reset(); from = target = out = 0.5f; rate.invalidate(); }
    float phase() const override { return std::min ((float) clk.frac, 0.9999f); }

    float process (const ControlContext& c, const ParamInputs& p) override
    {
        followRate (c);
        const int n = c.numSamples;
        if (n <= 0) return out;
        const bool walk = p.mode[0] == 1;
        const bool sync = p.mode[1] == 1;
        const Transport& t = transportOf (c);

        StepClock::Tick tick;
        if (sync && t.playing)
            tick = clk.lock (endPpq (t, n) / (double) maps::syncRateBeats (safe01 (p.v[0][n - 1])));
        else
            tick = clk.advance (sync ? syncCycles (p.v[0], 0, n, samplesPerBeat (t))
                                     : freeCycles (p.v[0], 0, n, sr, rate, [] (float v) { return maps::randRateHz (v); }));

        if (tick != StepClock::None)
        {
            from = tick == StepClock::Next ? target : out;   // a relocation starts from where we are
            float next = from;
            if (rng.uniform() < safe01 (p.v[2][n - 1]))
            {
                const float range = safe01 (p.v[3][n - 1]);
                if (walk)
                {
                    const float lo = 0.5f - 0.5f * range, hi = 0.5f + 0.5f * range;
                    float x = dsp::clampf (from, lo, hi) + 0.25f * range * rng.bipolar();
                    if (x > hi) x = 2.0f * hi - x;
                    if (x < lo) x = 2.0f * lo - x;
                    next = dsp::clampf (x, lo, hi);
                }
                else next = 0.5f + (rng.uniform() - 0.5f) * range;
            }
            target = next;
        }

        // SMOOTH: raised-cosine glide over that fraction of the tick (0 = steps, 1 = continuous)
        const float smooth = safe01 (p.v[1][n - 1]);
        float w = 1.0f;
        if (smooth > 1.0e-3f) w = 0.5f - 0.5f * std::cos (kPi * clamp01 ((float) clk.frac / smooth));
        out = clamp01 (from + (target - from) * w);
        return out;
    }
};

// ── SEQ ─────────────────────────────────────────────────────────────────────────────────────────

struct Seq final : ControlBase
{
    const uint32_t seed = nextSeed();
    dsp::Rng rng { seed };
    StepClock clk;
    float from = 0.0f, target = 0.0f, out = 0.0f;
    int index = 0, steps = 8;
    bool pending = true;    // fire the current step at the next process (after reset / note-on)
    bool fresh = true;      // no glide into the very first value
    MapCache rate;
    NoteEvents ev;

    void rateChanged() override { rate.invalidate(); }
    void reset() override
    {
        rng = dsp::Rng (seed); clk.reset(); from = target = out = 0.0f;
        index = 0; steps = 8; pending = fresh = true; rate.invalidate(); ev.clear();
    }
    void noteOn (const NoteState&) override { ev.noteOn(); }
    float phase() const override { return std::min (((float) index + (float) clk.frac) / (float) std::max (1, steps), 0.9999f); }

    int stepIndex (int dir, int64_t k, int count) const noexcept
    {
        if (count <= 1) return 0;
        switch (dir)
        {
            case 1:  return count - 1 - (int) modFloor (k, count);                                     // REV
            case 2:  { const int64_t q = modFloor (k, 2 * count - 2); return (int) (q < count ? q : 2 * count - 2 - q); } // PING
            case 3:  return (int) (hash32 ((uint64_t) k ^ ((uint64_t) seed << 32)) % (uint32_t) count); // RAND
            default: return (int) modFloor (k, count);                                                 // FWD
        }
    }

    float process (const ControlContext& c, const ParamInputs& p) override
    {
        followRate (c);
        const int n = c.numSamples;
        if (n <= 0) return out;
        const int dir = clampi (p.mode[0], 0, 3);
        const bool sync = p.mode[1] == 1;
        const float* aux = auxOf (c);
        const Transport& t = transportOf (c);
        const auto s = ev.take (c);
        steps = maps::seqSteps (safe01 (p.v[1][n - 1]));

        StepClock::Tick tick;
        if (sync && t.playing)
            tick = clk.lock (endPpq (t, n) / (double) maps::syncRateBeats (safe01 (p.v[0][n - 1])));
        else
        {
            int start = 0;
            if (s.onAt >= 0) { clk.reset(); pending = true; start = s.onAt; }   // free-running: restart at step 0
            tick = clk.advance (sync ? syncCycles (p.v[0], start, n, samplesPerBeat (t))
                                     : freeCycles (p.v[0], start, n, sr, rate, [] (float v) { return maps::randRateHz (v); }));
        }

        if (tick != StepClock::None || pending)
        {
            const float cur = (tick == StepClock::Next && ! pending) ? target : out;
            index = stepIndex (dir, clk.step, steps);
            float next = cur;   // a step that does not fire holds the previous value
            if (rng.uniform() < safe01 (p.v[3][n - 1]) * safe01 (aux[8 + index])) next = safe01 (aux[index]);
            from = fresh ? next : cur;
            target = next;
            pending = fresh = false;
        }

        // GLIDE: linear portamento over that fraction of the step
        const float glide = safe01 (p.v[2][n - 1]);
        const float w = glide > 1.0e-3f ? clamp01 ((float) clk.frac / glide) : 1.0f;
        out = clamp01 (from + (target - from) * w);
        return out;
    }
};

// ── FOLLOW ──────────────────────────────────────────────────────────────────────────────────────

struct Follow final : ControlBase
{
    float env = 0.0f, ms = 0.0f, out = 0.0f;
    float msCoef = 0.0f;      // 10 ms mean-square window for RMS detection
    MapCache atk, rel;

    Follow() { rateChanged(); }
    void rateChanged() override { atk.invalidate(); rel.invalidate(); msCoef = 1.0f - std::exp (-1.0f / (0.01f * sr)); }
    void reset() override { env = ms = out = 0.0f; }

    float process (const ControlContext& c, const ParamInputs& p) override
    {
        followRate (c);
        const int n = c.numSamples;
        if (n <= 0) return out;
        const int src = clampi (p.mode[0], 0, 4);
        const bool rms = p.mode[1] == 1;

        const float* x = nullptr;
        int len = 0;
        if (src < 4) { if (c.laneSignal) { x = c.laneSignal[src]; len = c.laneSignalLength; } }
        else if (c.sidechain) { x = c.sidechain; len = n; }
        if (x == nullptr) len = 0;
        len = clampi (len, 0, n);

        const float rate = sr;
        auto coef = [rate] (float sec) { return 1.0f - std::exp (-1.0f / (sec * rate)); };
        float aA = 0.0f, aR = 0.0f;
        float e = env, m = ms;
        for (int i = 0; i < n; ++i)
        {
            if ((i & (kChunk - 1)) == 0)
            {
                aA = atk.get (safe01 (p.v[0][i]), [&] (float v) { return coef (maps::followAttackSec (v)); });
                aR = rel.get (safe01 (p.v[1][i]), [&] (float v) { return coef (maps::followReleaseSec (v)); });
            }
            const float xi = i < len ? x[i] : 0.0f;
            float d;
            if (rms) { m += (xi * xi - m) * msCoef; d = std::sqrt (m); }
            else d = std::abs (xi);
            e += (d - e) * (d > e ? aA : aR);
        }
        if (! std::isfinite (e) || ! std::isfinite (m)) { e = 0.0f; m = 0.0f; }   // bad host input: recover
        env = e < 1.0e-12f ? 0.0f : std::min (e, 1000.0f);
        ms  = m < 1.0e-20f ? 0.0f : std::min (m, 1.0e6f);

        float db = env > 1.0e-9f ? 20.0f * std::log10 (env) : -180.0f;
        db += maps::followGainDb (safe01 (p.v[2][n - 1]));
        db = std::max (db, -60.0f + 60.0f * safe01 (p.v[3][n - 1]));
        out = clamp01 ((db + 60.0f) * (1.0f / 60.0f));
        return out;
    }
};

// ── DRIFT ───────────────────────────────────────────────────────────────────────────────────────
// Three Catmull-Rom interpolated random sources at incommensurate rates (1, √3, 3+√2).

struct Drift final : ControlBase
{
    struct Src { double ph = 0.0; float p[4] {}; };
    static constexpr double kMul[3] = { 1.0, 1.7320508075688772, 4.414213562373095 };

    const uint32_t seed = nextSeed();
    dsp::Rng rng { seed };
    Src src[3];
    float out = 0.5f;
    MapCache rate;
    int rateMode = -1;

    Drift() { reset(); }
    void rateChanged() override { rate.invalidate(); }
    void reset() override
    {
        rng = dsp::Rng (seed);
        for (auto& s : src) { s.ph = rng.uniform(); for (auto& v : s.p) v = rng.bipolar(); }
        out = 0.5f;
        rate.invalidate();
    }

    float process (const ControlContext& c, const ParamInputs& p) override
    {
        followRate (c);
        const int n = c.numSamples;
        if (n <= 0) return out;
        const int speed = clampi (p.mode[0], 0, 2);
        if (speed != rateMode) { rateMode = speed; rate.invalidate(); }
        const double cyc = freeCycles (p.v[0], 0, n, sr, rate, [speed] (float v) { return maps::driftRateHz (v, speed); });

        float val[3];
        for (int j = 0; j < 3; ++j)
        {
            Src& s = src[j];
            s.ph += cyc * kMul[j];
            for (int guard = 0; s.ph >= 1.0 && guard < 64; ++guard)
            {
                s.ph -= 1.0;
                s.p[0] = s.p[1]; s.p[1] = s.p[2]; s.p[2] = s.p[3]; s.p[3] = rng.bipolar();
            }
            if (! (s.ph >= 0.0 && s.ph < 1.0)) s.ph = 0.0;
            val[j] = catmullRom (s.p[0], s.p[1], s.p[2], s.p[3], (float) s.ph);
        }

        const float wander = safe01 (p.v[2][n - 1]);
        const float depth = safe01 (p.v[1][n - 1]);
        const float centre = 0.5f + 0.5f * maps::bipolar (safe01 (p.v[3][n - 1]));
        const float mix = (0.62f * val[0] + 0.38f * val[1] + 0.5f * wander * val[2]) / (1.0f + 0.5f * wander);
        out = clamp01 (centre + 0.5f * depth * dsp::softClip (1.4f * mix));
        return out;
    }
};

// ── LOGIC ───────────────────────────────────────────────────────────────────────────────────────

/** Block-rate one-pole slew: `maxSec` is the time to reach ~90 % at SLEW = 1 (quadratic taper). */
struct BlockSlew
{
    MapCache cache;
    int lastN = -1;
    float y = 0.0f;
    bool fresh = true;

    void reset() noexcept { y = 0.0f; fresh = true; cache.invalidate(); }
    float process (float target, float slew, float maxSec, int n, float sampleRate) noexcept
    {
        if (n != lastN) { lastN = n; cache.invalidate(); }
        const float k = cache.get (slew, [&] (float v) { return onePoleCoef (maxSec * v * v / 2.3f, (float) n, sampleRate); });
        if (fresh) { y = target; fresh = false; }
        else y += (target - y) * k;
        if (std::abs (y - target) < 1.0e-6f || ! std::isfinite (y)) y = target;
        return y;
    }
};

struct Logic final : ControlBase
{
    static constexpr float kHyst = 0.005f;   // ± around the threshold

    const uint32_t seed = nextSeed();
    dsp::Rng rng { seed };
    bool aHi = false, bHi = false, gtHi = false, raw = false, armed = false, gate = false;
    double holdLeft = 0.0;   // samples
    BlockSlew slew;

    void rateChanged() override { slew.cache.invalidate(); }
    void reset() override
    {
        rng = dsp::Rng (seed);
        aHi = bHi = gtHi = raw = armed = gate = false;
        holdLeft = 0.0;
        slew.reset();
    }

    float process (const ControlContext& c, const ParamInputs& p) override
    {
        followRate (c);
        const int n = c.numSamples;
        if (n <= 0) return slew.y;
        const float* aux = auxOf (c);
        const float A = inputValue (c, aux[0]), B = inputValue (c, aux[1]);
        const float th = safe01 (p.v[0][n - 1]);
        aHi  = aHi  ? A > th - kHyst : A > th + kHyst;
        bHi  = bHi  ? B > th - kHyst : B > th + kHyst;
        gtHi = gtHi ? A > B - kHyst  : A > B + kHyst;

        bool r;
        switch (clampi (p.mode[0], 0, 3))
        {
            case 0:  r = aHi && bHi; break;
            case 1:  r = aHi || bHi; break;
            case 2:  r = aHi != bHi; break;
            default: r = gtHi; break;
        }

        holdLeft = std::max (0.0, holdLeft - (double) n);
        if (r && ! raw)   // rising edge: passes with PROB
        {
            armed = rng.uniform() < safe01 (p.v[2][n - 1]);
            if (armed)
            {
                gate = true;
                const float hv = safe01 (p.v[3][n - 1]);
                holdLeft = hv > 0.001f ? (double) maps::envTimeSec (hv) * 0.1 * (double) sr : 0.0;
            }
        }
        if (! r) armed = false;
        raw = r;
        if (gate && ! armed && holdLeft <= 0.0) gate = false;

        return clamp01 (slew.process (gate ? 1.0f : 0.0f, safe01 (p.v[1][n - 1]), 0.2f, n, sr));
    }
};

// ── MATH ────────────────────────────────────────────────────────────────────────────────────────

struct MathMod final : ControlBase
{
    BlockSlew slew;

    void rateChanged() override { slew.cache.invalidate(); }
    void reset() override { slew.reset(); }

    float process (const ControlContext& c, const ParamInputs& p) override
    {
        followRate (c);
        const int n = c.numSamples;
        if (n <= 0) return slew.y;
        const float* aux = auxOf (c);
        const float a = inputValue (c, aux[0]) * safe01 (p.v[0][n - 1]);
        const float b = inputValue (c, aux[1]) * safe01 (p.v[1][n - 1]);
        float r;
        switch (clampi (p.mode[0], 0, 4))
        {
            case 0:  r = clamp01 (a + b); break;
            case 1:  r = std::min (a, b); break;
            case 2:  r = std::max (a, b); break;
            case 3:  r = a * b; break;
            default: r = std::abs (a - b); break;
        }
        const int q = (int) std::lround (safe01 (p.v[2][n - 1]) * 16.0f);
        if (q > 0) r = std::round (r * (float) q) / (float) q;
        return clamp01 (slew.process (r, safe01 (p.v[3][n - 1]), 1.0f, n, sr));
    }
};

// ── CLOCK ───────────────────────────────────────────────────────────────────────────────────────

struct Clock final : ControlBase
{
    StepClock clk;
    MapCache rate;
    uint32_t pattern = 0;
    int patN = -1, patK = -1;
    float out = 0.0f, ph = 0.0f;

    void rateChanged() override { rate.invalidate(); }
    void reset() override { clk.reset(); rate.invalidate(); out = 0.0f; ph = 0.0f; }
    float phase() const override { return ph; }

    float process (const ControlContext& c, const ParamInputs& p) override
    {
        followRate (c);
        const int n = c.numSamples;
        if (n <= 0) return out;
        const bool euclid = p.mode[0] == 1;
        const bool sync = p.mode[1] == 1;
        const Transport& t = transportOf (c);
        if (sync && t.playing)
            clk.lock (endPpq (t, n) / (double) maps::syncRateBeats (safe01 (p.v[0][n - 1])));
        else
            clk.advance (sync ? syncCycles (p.v[0], 0, n, samplesPerBeat (t))
                              : freeCycles (p.v[0], 0, n, sr, rate, [] (float v) { return maps::randRateHz (v); }));

        const int steps = maps::clockSteps (safe01 (p.v[1][n - 1]));
        const int fill = maps::clockFill (safe01 (p.v[2][n - 1]), steps);
        if (steps != patN || fill != patK) { pattern = bjorklund (steps, fill); patN = steps; patK = fill; }

        // SWING: every second step starts later by up to half a step; its pulse keeps half its length
        const double d = 0.5 * (double) safe01 (p.v[3][n - 1]);
        const int64_t k = clk.step;
        const double f = clk.frac;
        const int64_t par = modFloor (k, 2);
        const double u = (double) par + f;   // position inside the pair of steps, [0,2)
        int64_t logical;
        bool high;
        if (u < 1.0 + d) { logical = k - par;     high = u < 0.5; }
        else             { logical = k - par + 1; high = (u - 1.0 - d) < 0.5 * (1.0 - d); }
        if (euclid) high = high && ((pattern >> (int) modFloor (logical, steps)) & 1u) != 0u;

        ph = euclid ? (float) (((double) modFloor (k, steps) + f) / (double) steps) : (float) f;
        ph = std::min (std::max (ph, 0.0f), 0.9999f);
        out = high ? 1.0f : 0.0f;
        return out;
    }
};

// ── PHYSICS ─────────────────────────────────────────────────────────────────────────────────────
// GRAVITY sets the time scale T = 2 s .. 50 ms: BALL falls from the top in T, PENDULUM swings with
// a period of 2T, SPRING rings with a period of T.

struct Physics final : ControlBase
{
    enum Type : int { kBall = 0, kPendulum = 1, kSpring = 2 };
    static constexpr float kThetaMax = 1.5f;   // radians at KICK = 1

    float h = 0.0f, vel = 0.0f;   // ball height / velocity (units per second)
    float th = 0.0f, om = 0.0f;   // pendulum angle / angular velocity
    float x = 0.0f, xv = 0.0f;    // spring displacement / velocity
    bool resting = true;
    int lastType = -1;
    float out = 0.0f;
    MapCache tscale;
    NoteEvents ev;

    // per-chunk coefficients
    float g = 0.0f, drag = 1.0f, rest = 0.0f, vRest = 0.0f, w0 = 1.0f, zeta = 0.0f;

    void rateChanged() override { tscale.invalidate(); }
    void reset() override
    {
        h = vel = th = om = x = xv = 0.0f;
        resting = true; lastType = -1; out = 0.0f; ev.clear();
    }
    void noteOn (const NoteState&) override { ev.noteOn(); }

    void refresh (int type, const ParamInputs& p, int i) noexcept
    {
        const float T = tscale.get (safe01 (p.v[0][i]), [] (float v) { return 2.0f * std::pow (0.025f, v); });
        const float damp = safe01 (p.v[3][i]);
        const float dt = 1.0f / sr;
        if (type == kBall)
        {
            g = 2.0f / (T * T);
            drag = std::max (0.0f, 1.0f - damp * 1.5f / T * dt);
            rest = 0.3f + 0.68f * safe01 (p.v[1][i]);
            vRest = std::sqrt (2.0f * g * 1.0e-3f);   // rebounds lower than 0.1 % come to rest
        }
        else
        {
            w0 = (type == kPendulum ? kPi : kTwoPi) / T;
            zeta = type == kPendulum ? 0.003f + 0.3f * damp * damp : 0.01f + 0.5f * damp * damp;
        }
    }

    void kick (int type, float amount) noexcept
    {
        amount = safe01 (amount);
        if (amount <= 0.0f) return;
        if (type == kBall)
        {
            const float peak = std::min (1.0f, h + amount);
            const float kv = std::sqrt (std::max (0.0f, 2.0f * g * (peak - h)));
            if (kv > vel) vel = kv;
            if (vel > 0.0f) resting = false;
        }
        else if (type == kPendulum)
        {
            const float A = amount * kThetaMax;
            if (std::abs (th) < A)
            {
                const float c2 = 2.0f * (std::cos (th) - std::cos (A));
                om = (om >= 0.0f ? 1.0f : -1.0f) * w0 * std::sqrt (std::max (0.0f, c2));
                resting = false;
            }
        }
        else
        {
            xv += 0.5f * amount * w0;
            const float amp = std::sqrt (x * x + (xv / w0) * (xv / w0));
            if (amp > 0.5f) { const float k = 0.5f / amp; x *= k; xv *= k; }
            resting = false;
        }
    }

    float process (const ControlContext& c, const ParamInputs& p) override
    {
        followRate (c);
        const int n = c.numSamples;
        if (n <= 0) return out;
        const int type = clampi (p.mode[0], 0, 2);
        const bool freeRun = p.mode[1] == 1;
        if (type != lastType) { h = vel = th = om = x = xv = 0.0f; resting = true; lastType = type; }
        const auto s = ev.take (c);
        int next = 0;
        const float dt = 1.0f / sr;

        for (int i = 0; i < n; ++i)
        {
            if ((i & (kChunk - 1)) == 0) refresh (type, p, i);
            for (; next < s.count && s.e[next].at == i; ++next)
                if (s.e[next].isOn && ! freeRun) kick (type, p.v[2][i]);
            if (freeRun && resting) kick (type, p.v[2][i]);
            if (resting) continue;

            if (type == kBall)
            {
                vel = (vel - g * dt) * drag;
                h += vel * dt;
                if (h <= 0.0f)
                {
                    h = -h * rest;
                    if (vel < 0.0f) vel = -vel * rest;
                    if (vel < vRest) { vel = 0.0f; h = 0.0f; resting = true; }
                }
                else if (h > 1.0f) { h = 1.0f; if (vel > 0.0f) vel = -vel * rest; }
            }
            else if (type == kPendulum)
            {
                om += (-w0 * w0 * sinSmall (th) - 2.0f * zeta * w0 * om) * dt;
                th += om * dt;
                if (th > 0.5f * kPi)  { th = 0.5f * kPi;  om = std::min (om, 0.0f); }
                if (th < -0.5f * kPi) { th = -0.5f * kPi; om = std::max (om, 0.0f); }
            }
            else
            {
                xv += (-w0 * w0 * x - 2.0f * zeta * w0 * xv) * dt;
                x += xv * dt;
            }
        }

        if (! std::isfinite (h + vel + th + om + x + xv)) { h = vel = th = om = x = xv = 0.0f; resting = true; }
        if (! resting && type != kBall)
        {
            const float a = type == kPendulum ? th : x;
            const float b = (type == kPendulum ? om : xv) / w0;
            if (a * a + b * b < 1.0e-8f) { th = om = x = xv = 0.0f; resting = true; }
        }

        if (type == kBall) out = clamp01 (h);
        else if (type == kPendulum) out = clamp01 (0.5f + 0.5f * sinSmall (th) * (1.0f / 0.99749499f));   // / sin(1.5)
        else out = clamp01 (0.5f + x);
        return out;
    }
};

} // namespace

std::unique_ptr<ControlModule> createControlModuleImpl (ModuleType t)
{
    switch (t)
    {
        case ModuleType::Lfo:     return std::make_unique<Lfo>();
        case ModuleType::Env:     return std::make_unique<Env>();
        case ModuleType::Rand:    return std::make_unique<Rand>();
        case ModuleType::Seq:     return std::make_unique<Seq>();
        case ModuleType::Follow:  return std::make_unique<Follow>();
        case ModuleType::Drift:   return std::make_unique<Drift>();
        case ModuleType::Logic:   return std::make_unique<Logic>();
        case ModuleType::Math:    return std::make_unique<MathMod>();
        case ModuleType::Clock:   return std::make_unique<Clock>();
        case ModuleType::Physics: return std::make_unique<Physics>();
        default: return nullptr;
    }
}
} // namespace weft
