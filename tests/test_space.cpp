// SPACE family: robustness, timing, decay, pitch, freeze passthrough.
#include "Test.h"
#include "Fuzz.h"
#include "engine/ParamMaps.h"

using namespace weft;
using namespace wtest;

namespace
{
std::vector<std::vector<float>> runFx (ModuleType t, const std::vector<float>& inL, const std::vector<float>& inR,
                                       std::array<float, 4> params, std::array<int, 2> modes, int ch = 2, float sr = 48000.0f)
{
    auto m = createAudioModule (t);
    m->prepare (sr);
    AudioRun r; r.type = t; r.channels = ch; r.sampleRate = sr;
    for (int k = 0; k < 4; ++k) r.params[k] = params[(size_t) k];
    r.modes[0] = modes[0]; r.modes[1] = modes[1];
    r.transport.samplesPerBeat = sr * 0.5f;
    return r.run (*m, (int) inL.size(), inL.data(), inR.empty() ? nullptr : inR.data());
}

int firstAbove (const std::vector<float>& x, size_t from, float thr)
{
    for (size_t i = from; i < x.size(); ++i) if (std::abs (x[i]) > thr) return (int) i;
    return -1;
}
} // namespace

TEST ("space: robustness of every module")
{
    for (auto t : { ModuleType::Delay, ModuleType::Verb, ModuleType::Shift, ModuleType::Freeze, ModuleType::Chorus, ModuleType::Loop })
        robustnessSweep (t, 6, 4.0);
}

TEST ("space: DELAY echo at the expected offset (free and synced)")
{
    std::vector<float> imp (48000, 0.0f);
    imp[100] = 1.0f;
    // free: delayFreeMs(v) = 2000^v ms -> 250 ms
    const float v = std::log (250.0f) / std::log (2000.0f);
    for (int type : { 1, 2 })
    {
        auto o = runFx (ModuleType::Delay, imp, imp, { v, 0.0f, 0.5f, 1.0f }, { type, 0 });
        const int at = firstAbove (o[0], 101, type == 2 ? 0.05f : 0.3f);
        std::printf ("    delay type %d echo at %d (expected %d)\n", type, at, 100 + 12000);
        CHECK (type == 2 ? std::abs (at - 12100) < 80 : std::abs (at - 12100) <= 1);
    }
    // sync 1/8 at 120 bpm = 0.5 beat = 12000 samples
    float sv = 0.0f;
    for (int i = 0; i < dsp::kNumDivisions; ++i) if (dsp::kDivisions[i].beats == 0.5f) sv = (float) i / (dsp::kNumDivisions - 1);
    auto o = runFx (ModuleType::Delay, imp, imp, { sv, 0.0f, 0.5f, 1.0f }, { 1, 1 });
    CHECK (std::abs (firstAbove (o[0], 101, 0.3f) - 12100) <= 1);
    // ping: the first echo is on the left, the second on the right
    auto pp = runFx (ModuleType::Delay, imp, imp, { v, 0.8f, 0.5f, 1.0f }, { 0, 0 });
    CHECK (std::abs (firstAbove (pp[0], 101, 0.2f) - 12100) <= 1);
    CHECK (std::abs (firstAbove (pp[1], 101, 0.2f) - 24100) <= 1);
}

TEST ("space: DELAY at full feedback stays bounded for 20 s")
{
    const auto in = noise (48000 * 20, 0.5f);
    for (int type = 0; type < 3; ++type)
    {
        auto o = runFx (ModuleType::Delay, in, in, { 0.3f, 1.0f, 0.5f, 1.0f }, { type, 0 });
        CHECK (stats (o[0]).finite && stats (o[0]).peak < 4.0);
    }
}

TEST ("space: VERB decay roughly matches T60 and is decorrelated")
{
    const float sr = 48000.0f;
    for (float size : { 0.2f, 0.5f, 0.7f })
    {
        const float t60 = maps::verbDecaySec (size);
        const int n = (int) (sr * (t60 * 1.2f + 1.0f));
        std::vector<float> in ((size_t) n, 0.0f);
        const auto burst = noise (2400, 0.5f);
        std::copy (burst.begin(), burst.end(), in.begin());
        auto o = runFx (ModuleType::Verb, in, in, { size, 0.0f, 0.0f, 1.0f }, { 1, 0 });
        // energy envelope in 50 ms windows: find the -30 dB point after the peak (T30 * 2)
        std::vector<double> env;
        for (size_t i = 0; i + 2400 < o[0].size(); i += 2400)
        {
            double e = 0; for (size_t k = i; k < i + 2400; ++k) e += o[0][k] * o[0][k] + o[1][k] * o[1][k];
            env.push_back (10.0 * std::log10 (e + 1e-20));
        }
        const auto pk = std::max_element (env.begin(), env.end());
        int idx = -1;
        for (auto it = pk; it != env.end(); ++it) if (*it < *pk - 30.0) { idx = (int) (it - env.begin()); break; }
        const double est = idx < 0 ? 1e9 : (idx - (pk - env.begin())) * 0.05 * 2.0;
        std::printf ("    verb size %.2f t60 %.2f s estimated %.2f s\n", size, t60, est);
        CHECK (est > t60 * 0.7 && est < t60 * 1.3);
        // decorrelation
        double lr = 0, ll = 0, rr = 0;
        for (size_t i = 4800; i < 4800 * 4; ++i) { lr += o[0][i] * o[1][i]; ll += o[0][i] * o[0][i]; rr += o[1][i] * o[1][i]; }
        CHECK (std::abs (lr / std::sqrt (ll * rr)) < 0.5);
    }
}

TEST ("space: SHIFT +12 st doubles a sine")
{
    const float sr = 48000.0f;
    const auto in = sine (440.0f, sr, 48000, 0.5f);
    for (int type = 0; type < 2; ++type)
    {
        auto o = runFx (ModuleType::Shift, in, in, { 0.75f, 0.0f, 0.0f, 1.0f }, { type, 1 });
        const double hz = dominantHz (o[0], sr, 300.0f, 1200.0f, 9600);
        std::printf ("    shift type %d dominant %.1f Hz\n", type, hz);
        CHECK_NEAR (hz / 880.0, 1.0, 0.03);
    }
}

TEST ("space: FREEZE passes through bit-identically and sustains when frozen")
{
    const auto in = noise (48000, 0.5f);
    auto o = runFx (ModuleType::Freeze, in, in, { 0.0f, 0.5f, 0.6f, 1.0f }, { 0, 0 });
    bool same = true;
    for (size_t i = 0; i < in.size(); ++i) same &= o[0][i] == in[i] && o[1][i] == in[i];
    CHECK (same);
    // engage after 0.5 s of material, then silence
    auto m = createAudioModule (ModuleType::Freeze);
    m->prepare (48000.0);
    AudioRun r; r.type = ModuleType::Freeze; r.channels = 2;
    r.params[0] = 0.0f; r.params[1] = 0.0f; r.params[2] = 0.5f; r.params[3] = 1.0f;
    const auto tone = sine (330.0f, 48000.0f, 24000, 0.5f);
    r.run (*m, 24000, tone.data(), tone.data());
    r.params[0] = 1.0f;
    std::vector<float> silence (96000, 0.0f);
    const auto held = r.run (*m, 96000, silence.data(), silence.data(), false);
    std::vector<float> tail (held[0].end() - 24000, held[0].end());
    std::printf ("    frozen tail rms %.3f\n", stats (tail).rms);
    CHECK (stats (tail).rms > 0.05);
    // the engine's lane freeze settings
    const float fadeSec = maps::freezeFadeSec (0.28f);
    CHECK (fadeSec > 0.1f && fadeSec < 0.4f);
}

TEST ("space: CHORUS and FLANGE modulate")
{
    const float sr = 48000.0f;
    const auto in = sine (1000.0f, sr, 96000, 0.4f);
    for (int type = 0; type < 3; ++type)
    {
        auto o = runFx (ModuleType::Chorus, in, in, { 0.6f, 1.0f, 0.3f, 1.0f }, { type, 0 });
        // amplitude envelope varies over time (beating between voices / comb sweep)
        double mn = 1e9, mx = 0;
        for (size_t i = 9600; i + 480 < o[0].size(); i += 480)
        {
            double e = 0; for (size_t k = i; k < i + 480; ++k) e += o[0][k] * o[0][k];
            mn = std::min (mn, e); mx = std::max (mx, e);
        }
        std::printf ("    chorus type %d env ratio %.2f\n", type, mx / std::max (1e-9, mn));
        CHECK (mx / std::max (1e-9, mn) > 1.1);
        CHECK (stats (o[0]).peak < 2.0);
    }
}

TEST ("space: LOOP reproduces recorded material and reverses")
{
    const float sr = 48000.0f;
    // length 1 s: loopLengthSec(v) = 0.1 * 80^v
    const float lv = std::log (10.0f) / std::log (80.0f);
    std::vector<float> ramp (48000);
    for (int i = 0; i < 48000; ++i) ramp[(size_t) i] = 0.5f * std::sin (2.0f * 3.14159f * 3.0f * i / sr) * (float) i / 48000.0f;
    auto m = createAudioModule (ModuleType::Loop);
    m->prepare (sr);
    AudioRun r; r.type = ModuleType::Loop; r.channels = 1;
    r.params[0] = 0.75f; r.params[1] = lv; r.params[2] = 0.0f; r.params[3] = 1.0f; r.modes[0] = 0;
    r.run (*m, 48000, ramp.data());
    r.modes[0] = 1;   // play
    std::vector<float> silence (48000, 0.0f);
    const auto fwd = r.run (*m, 48000, silence.data(), nullptr, false);
    double err = 0; for (size_t i = 1000; i < 47000; ++i) err += std::abs (fwd[0][i] - ramp[i]);
    std::printf ("    loop fwd mean err %.4f\n", err / 46000);
    CHECK (err / 46000 < 0.02);
    r.params[0] = 0.25f;   // -1x
    const auto rev = r.run (*m, 48000, silence.data(), nullptr, false);
    // reverse playback: correlate with the time-reversed recording
    double best = 0;
    for (int lag = 0; lag < 48000; lag += 50)
    {
        double c = 0;
        for (int i = 0; i < 48000; i += 7) c += rev[0][(size_t) i] * ramp[(size_t) ((lag - i + 48000 * 2) % 48000)];
        best = std::max (best, c);
    }
    double e = 0; for (size_t i = 0; i < 48000; i += 7) e += ramp[i] * ramp[i];
    std::printf ("    loop reverse corr %.3f\n", best / e);
    CHECK (best / e > 0.8);
}

TEST ("space: benchmarks")
{
    for (auto t : { ModuleType::Delay, ModuleType::Verb, ModuleType::Shift, ModuleType::Freeze, ModuleType::Chorus, ModuleType::Loop })
        benchmark (t, 2);
    const int cloud[2] = { 2, 0 };
    benchmark (ModuleType::Verb, 2, nullptr, cloud);
}
