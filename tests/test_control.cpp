// CONTROL family: ranges, timing and behaviour of every modulator.
#include "Test.h"
#include "Fuzz.h"
#include "engine/ParamMaps.h"
#include "engine/modules/ControlDsp.h"

using namespace weft;
using namespace wtest;

namespace
{
std::unique_ptr<ControlModule> make (ModuleType t, float sr = 48000.0f)
{
    auto m = createControlModule (t);
    m->prepare (sr);
    m->reset();
    return m;
}

ControlRun setup (ModuleType t, std::array<float, 4> p, std::array<int, 2> modes)
{
    ControlRun r; r.type = t;
    for (int k = 0; k < 4; ++k) r.params[k] = p[(size_t) k];
    r.modes[0] = modes[0]; r.modes[1] = modes[1];
    return r;
}

/** Index of the first block whose value crosses `thr` going up (or down). */
int firstCross (const std::vector<float>& v, float thr, bool up, int from = 0)
{
    for (int i = std::max (1, from); i < (int) v.size(); ++i)
        if (up ? (v[(size_t) i] >= thr) : (v[(size_t) i] <= thr)) return i;
    return -1;
}
} // namespace

TEST ("control: every modulator stays in [0,1] under random modulation")
{
    dsp::Rng rng (77);
    for (auto t : controlModuleTypes())
    {
        for (const auto& modes : modeCombos (t))
            for (float sr : { 44100.0f, 96000.0f })
                for (int trial = 0; trial < 6; ++trial)
                {
                    auto m = make (t, sr);
                    ControlRun r; r.type = t; r.sampleRate = sr;
                    for (auto& p : r.params) p = rng.uniform();
                    for (auto& a : r.aux) a = rng.uniform() * 15.0f;
                    for (auto& s : r.slotValues) s = rng.uniform();
                    r.modes[0] = modes[0]; r.modes[1] = modes[1];
                    r.transport.playing = (trial & 1) != 0;
                    r.laneSignal[1] = noise (9600, 0.5f);
                    bool ok = true;
                    for (int rep = 0; rep < 4; ++rep)
                    {
                        for (auto& p : r.params) p = dsp::clamp01 (p + 0.3f * rng.bipolar());
                        const auto out = r.run (*m, 9600, rep == 0, rep == 2 ? 4800 : -1);
                        for (float v : out) ok &= std::isfinite (v) && v >= 0.0f && v <= 1.0f;
                        const float ph = m->phase();
                        ok &= ph == -1.0f || (ph >= 0.0f && ph < 1.0f + 1e-6f);
                    }
                    CHECK_MSG (ok, getSpec (t).name);
                }
    }
}

TEST ("control: LFO frequency matches its rate and sync locks to the host")
{
    auto m = make (ModuleType::Lfo);
    const float v = 0.4f;   // lfoRateHz
    auto r = setup (ModuleType::Lfo, { v, 0.5f, 0.0f, 0.0f }, { 2, 0 });   // SAW free
    const auto out = r.run (*m, 48000 * 20, false);
    int wraps = 0;
    for (size_t i = 1; i < out.size(); ++i) if (out[i] < out[i - 1] - 0.5f) ++wraps;
    const double hz = wraps / 20.0;
    CHECK_NEAR (hz / maps::lfoRateHz (v), 1.0, 0.05 + 1.0 / (20.0 * maps::lfoRateHz (v)));
    // sync: 1/4 at 120 bpm while playing -> the phase follows ppq exactly
    auto ms = make (ModuleType::Lfo);
    float sv = 0.0f;
    for (int i = 0; i < maps::kNumSyncRates; ++i) if (maps::kSyncRates[i].beats == 1.0f) sv = (float) i / (maps::kNumSyncRates - 1);
    auto rs = setup (ModuleType::Lfo, { sv, 0.5f, 0.0f, 0.0f }, { 2, 1 });
    rs.transport.playing = true; rs.transport.ppq = 0.25;
    rs.run (*ms, 32 * 10, false);
    const double ppqEnd = rs.transport.ppq;
    CHECK_NEAR (ms->phase(), ppqEnd - std::floor (ppqEnd), 0.01);
}

TEST ("control: ENV stage times, AD ignores gate, LOOP cycles")
{
    // ADSR LIN: attack 100 ms
    const float a = std::log (0.1f / 0.0005f) / std::log (20000.0f);
    auto m = make (ModuleType::Env);
    auto r = setup (ModuleType::Env, { a, 0.5f, 1.0f, 0.5f }, { 0, 1 });
    const auto out = r.run (*m, 48000);
    const int i90 = firstCross (out, 0.9f, true);
    const double t90 = i90 * 32.0 / 48000.0;
    CHECK_NEAR (t90, 0.09, 0.006);
    // release (LIN): from 1 to 0 in R
    const float rv = std::log (0.2f / 0.001f) / std::log (20000.0f);
    auto m2 = make (ModuleType::Env);
    auto r2 = setup (ModuleType::Env, { 0.0f, 0.5f, 1.0f, rv }, { 0, 1 });
    const auto o2 = r2.run (*m2, 48000, true, 9600);
    const int rel = firstCross (o2, 0.1f, false, 300);
    const double tr = (rel - 300) * 32.0 / 48000.0;
    CHECK_NEAR (tr, 0.18, 0.012);
    // AD ignores the gate: no sustain, decays to 0 while the key is held
    auto m3 = make (ModuleType::Env);
    auto r3 = setup (ModuleType::Env, { 0.0f, 0.3f, 1.0f, 0.5f }, { 1, 0 });
    const auto o3 = r3.run (*m3, 48000);
    CHECK (o3.back() < 0.01f);
    // LOOP cycles while held
    auto m4 = make (ModuleType::Env);
    auto r4 = setup (ModuleType::Env, { 0.2f, 0.2f, 0.0f, 0.5f }, { 2, 0 });
    const auto o4 = r4.run (*m4, 48000 * 2);
    int rises = 0;
    for (size_t i = 1; i < o4.size(); ++i) if (o4[i - 1] < 0.5f && o4[i] >= 0.5f) ++rises;
    CHECK (rises >= 3);
}

TEST ("control: RAND S&H ticks and PROB 0 holds")
{
    auto m = make (ModuleType::Rand);
    auto r = setup (ModuleType::Rand, { 0.5f, 0.0f, 1.0f, 1.0f }, { 0, 0 });   // randRateHz(0.5) ~ 1.58 Hz
    const auto out = r.run (*m, 48000 * 4, false);
    int changes = 0;
    for (size_t i = 1; i < out.size(); ++i) if (out[i] != out[i - 1]) ++changes;
    CHECK (changes >= 4 && changes <= 9);
    auto m2 = make (ModuleType::Rand);
    auto r2 = setup (ModuleType::Rand, { 0.8f, 0.0f, 0.0f, 1.0f }, { 0, 0 });
    const auto o2 = r2.run (*m2, 48000 * 2, false);
    int c2 = 0;
    for (size_t i = 100; i < o2.size(); ++i) if (o2[i] != o2[i - 1]) ++c2;
    CHECK (c2 == 0);
}

TEST ("control: SEQ walks its steps in order")
{
    auto m = make (ModuleType::Seq);
    auto r = setup (ModuleType::Seq, { 0.6f, 3.0f / 7.0f, 0.0f, 1.0f }, { 0, 0 });   // 4 steps, free
    for (int i = 0; i < 8; ++i) { r.aux[i] = 0.1f * (float) (i + 1); r.aux[8 + i] = 1.0f; }
    const auto out = r.run (*m, 48000 * 3);
    std::vector<float> seq;
    for (float v : out) if (seq.empty() || std::abs (seq.back() - v) > 1e-4f) seq.push_back (v);
    CHECK (seq.size() >= 6);
    bool ordered = true;
    for (size_t i = 1; i < seq.size(); ++i)
    {
        const int a = (int) std::lround (seq[i - 1] * 10.0f), b = (int) std::lround (seq[i] * 10.0f);
        ordered &= (b == a + 1) || (a == 4 && b == 1);
    }
    CHECK (ordered);
}

TEST ("control: CLOCK euclid 3/8 is the Bjorklund pattern")
{
    CHECK (ctl::bjorklund (8, 3) != 0u);
    int pulses = 0; for (int i = 0; i < 8; ++i) pulses += (ctl::bjorklund (8, 3) >> i) & 1u;
    CHECK (pulses == 3);
    // maximally even: gaps of 3,3,2 in some rotation
    std::vector<int> on; for (int i = 0; i < 8; ++i) if ((ctl::bjorklund (8, 3) >> i) & 1u) on.push_back (i);
    std::vector<int> gaps { on[1] - on[0], on[2] - on[1], 8 - on[2] + on[0] };
    std::sort (gaps.begin(), gaps.end());
    CHECK (gaps[0] == 2 && gaps[1] == 3 && gaps[2] == 3);
}

TEST ("control: LOGIC truth tables and MATH ops")
{
    const float A = 0.9f, B = 0.1f;
    const int ops[4] = { 0, 1, 2, 3 };
    const float expect[4] = { 0.0f, 1.0f, 1.0f, 1.0f };   // AND, OR, XOR, A > B
    for (int o = 0; o < 4; ++o)
    {
        auto m = make (ModuleType::Logic);
        auto r = setup (ModuleType::Logic, { 0.5f, 0.0f, 1.0f, 0.0f }, { ops[o], 0 });
        r.aux[0] = 0.0f; r.aux[1] = 1.0f; r.slotValues[0] = A; r.slotValues[1] = B;
        const auto out = r.run (*m, 4800);
        CHECK_NEAR (out.back(), expect[o], 1e-4);
    }
    const float mexp[5] = { 1.0f, 0.1f, 0.9f, 0.09f, 0.8f };   // SUM (clamped), MIN, MAX, MUL, DIFF
    for (int o = 0; o < 5; ++o)
    {
        auto m = make (ModuleType::Math);
        auto r = setup (ModuleType::Math, { 1.0f, 1.0f, 0.0f, 0.0f }, { o, 0 });
        r.aux[0] = 0.0f; r.aux[1] = 1.0f; r.slotValues[0] = A; r.slotValues[1] = B;
        const auto out = r.run (*m, 4800);
        CHECK_NEAR (out.back(), mexp[o], 1e-3);
    }
}

TEST ("control: FOLLOW tracks the selected lane only")
{
    auto m = make (ModuleType::Follow);
    auto r = setup (ModuleType::Follow, { 0.2f, 0.3f, 0.333f, 0.0f }, { 1, 0 });   // lane B, peak
    r.laneSignal[1] = sine (200.0f, 48000.0f, 48000, 0.5f);
    std::fill (r.laneSignal[1].begin() + 24000, r.laneSignal[1].end(), 0.0f);
    r.laneSignal[0] = sine (200.0f, 48000.0f, 48000, 0.9f);
    const auto out = r.run (*m, 48000);
    CHECK (out[700] > 0.6f);
    CHECK (out.back() < 0.2f);
}

TEST ("control: PHYSICS ball bounces with decreasing peaks")
{
    auto m = make (ModuleType::Physics);
    auto r = setup (ModuleType::Physics, { 0.5f, 0.7f, 0.8f, 0.0f }, { 0, 0 });
    const auto out = r.run (*m, 48000 * 4);
    std::vector<float> peaks;
    for (size_t i = 1; i + 1 < out.size(); ++i) if (out[i] > out[i - 1] && out[i] >= out[i + 1] && out[i] > 0.02f) peaks.push_back (out[i]);
    CHECK (peaks.size() >= 3);
    if (peaks.size() >= 3) CHECK (peaks[2] < peaks[0]);
}
