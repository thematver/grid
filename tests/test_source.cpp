// SOURCE family: robustness, pitch accuracy, band-limiting, resources.
#include "Test.h"
#include "Fuzz.h"
#include "engine/ParamMaps.h"

using namespace weft;
using namespace wtest;

namespace
{
std::vector<float> runOsc (int wave, float note, float sr, int n, float shape = 0.0f, float detune = 0.0f)
{
    auto m = createAudioModule (ModuleType::Osc);
    m->prepare (sr);
    AudioRun r; r.type = ModuleType::Osc; r.channels = 1; r.sampleRate = sr;
    r.params[0] = 0.5f; r.params[1] = shape; r.params[2] = detune; r.params[3] = 0.0f;
    r.modes[0] = wave; r.note.pitch = note;
    return r.run (*m, n)[0];
}

/** Energy at non-harmonic frequencies relative to total (aliasing measure). */
double aliasRatio (const std::vector<float>& x, float f0, float sr)
{
    double harm = 0.0, other = 0.0;
    for (float f = 50.0f; f < sr * 0.5f - 100.0f; f += 37.0f)
    {
        const double m = toneMagHann (x, f, sr, 2000);
        const double h = f / f0;
        const double dist = std::abs (h - std::round (h)) * f0;
        if (dist < 60.0) harm += m * m; else other += m * m;
    }
    return other / std::max (1e-12, harm);
}
} // namespace

TEST ("source: robustness of every module")
{
    for (auto t : { ModuleType::Osc, ModuleType::Noise, ModuleType::Input, ModuleType::Pluck, ModuleType::In })
        robustnessSweep (t, 12, 4.0);
    robustnessSweep (ModuleType::Wave, 8, 4.0);
    auto rain = builtinSample ("rain_tin.wav");
    robustnessSweep (ModuleType::Grain, 8, 4.0, rain.get());
    robustnessSweep (ModuleType::Grain, 2, 4.0, nullptr);
}

TEST ("source: OSC fundamental within 1% for every wave")
{
    const float sr = 48000.0f;
    for (int w = 0; w < 4; ++w)
        for (float note : { 45.0f, 60.0f, 81.0f })
        {
            const auto x = runOsc (w, note, sr, 48000);
            const double hz = dominantHz (x, sr, dsp::noteToHz (note) * 0.8f, dsp::noteToHz (note) * 1.25f, 1000);
            CHECK_NEAR (hz / dsp::noteToHz (note), 1.0, 0.01);
            const auto s = stats (x, 100);
            CHECK (s.peak > 0.3 && s.peak < 0.75);
        }
}

TEST ("source: OSC square is band-limited at high notes")
{
    const float sr = 48000.0f;
    const auto x = runOsc (1, 100.0f, sr, 24000);   // E7 ~ 2.6 kHz
    // naive square for comparison
    std::vector<float> naive (x.size());
    const float f = dsp::noteToHz (100.0f);
    double ph = 0.0;
    for (auto& v : naive) { v = ph < 0.5 ? 0.5f : -0.5f; ph += f / sr; ph -= std::floor (ph); }
    const double a = aliasRatio (x, f, sr), b = aliasRatio (naive, f, sr);
    std::printf ("    alias ratio blep %.5f naive %.5f\n", a, b);
    CHECK (a < b * 0.25);
}

TEST ("source: OSC detune 0 is a single oscillator, sync and PW stay bounded")
{
    const auto x = runOsc (0, 60.0f, 48000.0f, 9600, 0.0f, 0.0f);
    CHECK (stats (x).peak <= 0.6);
    const auto y = runOsc (1, 60.0f, 48000.0f, 9600, 1.0f, 1.0f);
    CHECK (stats (y).finite && stats (y).peak < 1.5);
}

TEST ("source: WAVE mip selection keeps aliasing low at C7")
{
    const float sr = 48000.0f;
    auto m = createAudioModule (ModuleType::Wave);
    m->prepare (sr);
    AudioRun r; r.type = ModuleType::Wave; r.channels = 1; r.sampleRate = sr;
    r.params[0] = 1.0f; r.params[1] = 0.5f; r.params[2] = 0.5f; r.params[3] = 0.0f;
    r.modes[0] = 2;   // Harmonic, last frame: square-ish, rich spectrum
    r.note.pitch = 108.0f;
    const auto x = r.run (*m, 24000)[0];
    const double a = aliasRatio (x, dsp::noteToHz (108.0f), sr);
    std::printf ("    wave alias ratio %.5f\n", a);
    CHECK (a < 0.02);
    CHECK (stats (x, 100).peak > 0.2);
}

TEST ("source: NOISE colours tilt from white to brown")
{
    const float sr = 48000.0f;
    double hi[4];
    for (int c = 0; c < 4; ++c)
    {
        auto m = createAudioModule (ModuleType::Noise);
        m->prepare (sr);
        AudioRun r; r.type = ModuleType::Noise; r.channels = 1; r.sampleRate = sr;
        r.params[0] = c / 3.0f; r.params[1] = 1.0f; r.params[2] = 0.0f; r.params[3] = 0.0f;
        const auto x = r.run (*m, 48000)[0];
        // high band energy: first difference power relative to total
        double d = 0.0, e = 0.0;
        for (size_t i = 1; i < x.size(); ++i) { d += (x[i] - x[i - 1]) * (x[i] - x[i - 1]); e += x[i] * x[i]; }
        hi[c] = d / e;
        const auto s = stats (x);
        std::printf ("    noise colour %d rms %.3f peak %.3f hf %.4f\n", c, s.rms, s.peak, hi[c]);
        CHECK (s.rms > 0.05 && s.peak < 1.0);
    }
    CHECK (hi[0] > hi[1] && hi[1] > hi[2] && hi[2] > hi[3]);
}

TEST ("source: GRAIN plays a sample and passes through without one")
{
    auto rain = builtinSample ("rain_tin.wav");
    for (int ch = 1; ch <= 2; ++ch)
    {
        auto m = createAudioModule (ModuleType::Grain);
        m->prepare (48000.0);
        AudioRun r; r.type = ModuleType::Grain; r.channels = ch; r.useDefaults(); r.resource = rain.get();
        CHECK (stats (r.run (*m, 48000)[0]).rms > 0.01);
        auto m2 = createAudioModule (ModuleType::Grain);
        m2->prepare (48000.0);
        r.resource = nullptr;
        CHECK (stats (r.run (*m2, 4800)[0]).peak == 0.0);
    }
}

TEST ("source: PLUCK pitch within 3 cents and decays")
{
    const float sr = 48000.0f;
    for (float note : { 40.0f, 57.0f, 69.0f, 84.0f })
    {
        auto m = createAudioModule (ModuleType::Pluck);
        m->prepare (sr);
        AudioRun r; r.type = ModuleType::Pluck; r.channels = 1; r.sampleRate = sr;
        r.params[0] = 0.5f; r.params[1] = 0.6f; r.params[2] = 0.6f; r.params[3] = 0.0f;
        r.note.pitch = note;
        const auto x = r.run (*m, 48000)[0];
        const double target = dsp::noteToHz (note);
        const double hz = dominantHz (x, sr, (float) target * 0.9f, (float) target * 1.1f, 2000);
        const double cents = 1200.0 * std::log2 (hz / target);
        std::printf ("    pluck %.0f: %.2f Hz (%.2f ct)\n", note, hz, cents);
        CHECK (std::abs (cents) < 3.0);
        std::vector<float> a (x.begin(), x.begin() + 4800), b (x.end() - 4800, x.end());
        CHECK (stats (b).rms < stats (a).rms * 0.8);
        CHECK (stats (x).peak > 0.1 && stats (x).peak < 1.2);
    }
}

TEST ("source: IN sums lane inputs with gains")
{
    auto m = createAudioModule (ModuleType::In);
    m->prepare (48000.0);
    std::vector<float> a (256, 0.5f), b (256, 0.25f);
    AudioRun r; r.type = ModuleType::In; r.channels = 2;
    r.params[0] = 1.0f; r.params[1] = 0.5f; r.params[2] = 1.0f; r.params[3] = 1.0f;
    r.laneIn[0] = r.laneIn[1] = a.data();
    r.laneIn[2] = r.laneIn[3] = b.data();
    r.inboundMask = 0x3;
    const auto y = r.run (*m, 256);
    CHECK_NEAR (y[0][100], 0.5 + 0.125, 1e-6);
    CHECK_NEAR (y[1][100], 0.5 + 0.125, 1e-6);
}

TEST ("source: built-in resources are complete and finite")
{
    for (const auto& n : builtinSampleNames())
    {
        auto s = builtinSample (n);
        CHECK (s != nullptr);
        if (! s) continue;
        CHECK (s->frames() > 48000 * 3);
        CHECK (stats (s->data).finite && stats (s->data).rms > 0.005 && stats (s->data).peak <= 1.0);
    }
    for (int i = 0; i < 6; ++i)
    {
        auto t = builtinWavetable (i);
        CHECK (t && t->isWavetable && t->waveFrames == 32 && t->waveLevels == AudioResource::kWaveMipLevels);
        if (! t) continue;
        CHECK (t->data.size() == (size_t) 32 * AudioResource::kWaveMipLevels * AudioResource::kWaveFrameSize);
        CHECK (stats (t->data).finite && stats (t->data).peak <= 1.6);
        std::printf ("    table %d peak %.3f\n", i, stats (t->data).peak);
    }
    // pos 0 of Glass Bell is a pure sine
    auto gb = builtinWavetable (0);
    const float* f0 = gb->waveFrame (0, 0);
    CHECK_NEAR (f0[512], 1.0, 0.01);
    // a wavetable from a pitched sample
    std::vector<float> sig (48000);
    for (int i = 0; i < 48000; ++i) sig[(size_t) i] = 0.5f * std::sin (2.0f * 3.14159265f * 220.0f * i / 48000.0f) + 0.2f * std::sin (2.0f * 3.14159265f * 440.0f * i / 48000.0f);
    auto smp = makeSampleResource ("t.wav", 48000.0f, 1, sig);
    auto wt = makeWavetableFromSample (*smp, "t");
    CHECK (wt && wt->waveFrames > 1 && wt->waveFrames <= 64);
    if (wt) CHECK (stats (wt->data).finite);
}

TEST ("source: benchmarks")
{
    for (auto t : { ModuleType::Osc, ModuleType::Wave, ModuleType::Noise, ModuleType::Pluck, ModuleType::In })
        benchmark (t, 1);
    const int detuned[2] = { 0, 0 };
    benchmark (ModuleType::Osc, 1, nullptr, detuned);
    auto rain = builtinSample ("rain_tin.wav");
    benchmark (ModuleType::Grain, 1, rain.get());
    benchmark (ModuleType::Input, 2);
}
