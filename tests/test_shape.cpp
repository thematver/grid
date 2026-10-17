// SHAPE family: robustness and module-specific behaviour.
#include "Test.h"
#include "Fuzz.h"
#include "engine/ParamMaps.h"

using namespace weft;
using namespace wtest;

namespace
{
std::vector<float> runFx (ModuleType t, const std::vector<float>& in, std::array<float, 4> params, std::array<int, 2> modes,
                          float pitch = 60.0f, float sr = 48000.0f, int ch = 1)
{
    auto m = createAudioModule (t);
    m->prepare (sr);
    AudioRun r; r.type = t; r.channels = ch; r.sampleRate = sr;
    for (int k = 0; k < 4; ++k) r.params[k] = params[(size_t) k];
    r.modes[0] = modes[0]; r.modes[1] = modes[1];
    r.note.pitch = pitch;
    return r.run (*m, (int) in.size(), in.data(), in.data())[0];
}

double gainDb (ModuleType t, float hz, std::array<float, 4> params, std::array<int, 2> modes, float pitch = 60.0f)
{
    const float sr = 48000.0f;
    const auto in = sine (hz, sr, 48000, 0.1f);
    const auto out = runFx (t, in, params, modes, pitch);
    const double a = toneMagHann (out, hz, sr, 24000), b = toneMagHann (in, hz, sr, 24000);
    return 20.0 * std::log10 (std::max (1e-12, a / b));
}

/** Analytic |H| in dB of the UI's filter curve (bilinear SVF matches it closely below fs/4). */
double analyticDb (float cut, float res, int type, bool two, float hz)
{
    const double fc = maps::filterCutoffHz (cut), Q = maps::filterQ (res);
    // prewarped bilinear analogue of the TPT SVF
    const double sr = 48000.0;
    const double u = std::tan (3.14159265358979 * hz / sr) / std::tan (3.14159265358979 * fc / sr);
    const double den = std::pow (1.0 - u * u, 2.0) + std::pow (u / Q, 2.0);
    double m2 = type == 0 ? 1.0 / den : type == 1 ? std::pow (u / Q, 2.0) / den : type == 2 ? std::pow (u, 4.0) / den : std::pow (1.0 - u * u, 2.0) / den;
    double db = 10.0 * std::log10 (m2);
    return two ? db * 2.0 : db;
}
} // namespace

TEST ("shape: robustness of every module")
{
    for (auto t : { ModuleType::Filter, ModuleType::Fold, ModuleType::Drive, ModuleType::Ring, ModuleType::Comb, ModuleType::Formant })
        robustnessSweep (t, 10, 4.0);
}

TEST ("shape: FILTER slopes, types and analytic magnitude")
{
    const float cut = 0.4f;   // ~316 Hz
    const float fc = maps::filterCutoffHz (cut);
    CHECK (gainDb (ModuleType::Filter, fc * 10.0f, { cut, 0.0f, 0.0f, 0.0f }, { 0, 1 }) < -40.0);
    CHECK (gainDb (ModuleType::Filter, fc * 10.0f, { cut, 0.0f, 0.0f, 0.0f }, { 0, 0 }) < -20.0);
    CHECK (gainDb (ModuleType::Filter, fc / 10.0f, { cut, 0.0f, 0.0f, 0.0f }, { 2, 1 }) < -40.0);
    CHECK (gainDb (ModuleType::Filter, fc * 10.0f, { cut, 0.0f, 0.0f, 0.0f }, { 2, 1 }) > -1.0);
    // BP peaks near the cutoff
    const double bpAt = gainDb (ModuleType::Filter, fc, { cut, 0.5f, 0.0f, 0.0f }, { 1, 0 });
    CHECK (bpAt > gainDb (ModuleType::Filter, fc * 3.0f, { cut, 0.5f, 0.0f, 0.0f }, { 1, 0 }) + 6.0);
    CHECK (std::abs (bpAt) < 1.0);
    for (int type = 0; type < 4; ++type)
        for (bool two : { false, true })
            for (float res : { 0.0f, 0.38f, 0.7f })
            {
                const double m = gainDb (ModuleType::Filter, fc, { cut, res, 0.0f, 0.0f }, { type, two ? 1 : 0 });
                const double a = analyticDb (cut, res, type, two, fc);
                if (type != 3) CHECK_NEAR (m, a, 1.0);
            }
}

TEST ("shape: FILTER key tracking doubles the cutoff for +12 st")
{
    const float cut = 0.4f;
    const float fc = maps::filterCutoffHz (cut);
    const double a = gainDb (ModuleType::Filter, fc * 2.0f, { cut, 0.0f, 0.0f, 1.0f }, { 0, 1 }, 72.0f);
    const double b = gainDb (ModuleType::Filter, fc, { cut, 0.0f, 0.0f, 1.0f }, { 0, 1 }, 60.0f);
    CHECK_NEAR (a, b, 0.5);
}

TEST ("shape: FILTER self-oscillation is bounded")
{
    std::vector<float> in (96000, 0.0f);
    in[10] = 0.5f;
    const auto out = runFx (ModuleType::Filter, in, { 0.5f, 1.0f, 1.0f, 0.0f }, { 0, 1 });
    const auto s = stats (out);
    CHECK (s.finite && s.peak < 4.0);
    std::vector<float> tail (out.end() - 4800, out.end());
    std::printf ("    self-osc tail rms %.3f peak %.3f\n", stats (tail).rms, s.peak);
    CHECK (stats (tail).rms > 0.01);
}

TEST ("shape: FOLD adds harmonics and keeps the peak")
{
    const auto in = sine (220.0f, 48000.0f, 48000, 0.5f);
    const auto out = runFx (ModuleType::Fold, in, { 0.8f, 0.5f, 1.0f, 1.0f }, { 2, 0 });
    const double h1 = toneMagHann (out, 220.0f, 48000.0f, 9600), h3 = toneMagHann (out, 660.0f, 48000.0f, 9600), h5 = toneMagHann (out, 1100.0f, 48000.0f, 9600);
    const double in3 = toneMagHann (in, 660.0f, 48000.0f, 9600);
    CHECK (h3 + h5 > 0.05 * h1);
    CHECK (in3 < 1e-3);
    std::vector<float> tail (out.begin() + 9600, out.end());
    CHECK (stats (tail).peak < 0.8 && stats (tail).peak > 0.25);
}

TEST ("shape: DRIVE crush quantizes and analogue types keep loudness")
{
    const auto in = sine (110.0f, 48000.0f, 24000, 0.5f);
    const auto out = runFx (ModuleType::Drive, in, { 0.2f, 0.0f, 1.0f, 1.0f }, { 3, 0 });   // 2 bits
    int distinct = 0; std::vector<float> seen;
    for (size_t i = 1000; i < out.size(); ++i)
        if (std::find_if (seen.begin(), seen.end(), [&] (float v) { return std::abs (v - out[i]) < 1e-5f; }) == seen.end()) { seen.push_back (out[i]); ++distinct; }
    CHECK (distinct <= 5);
    for (int type = 0; type < 3; ++type)
    {
        const auto o = runFx (ModuleType::Drive, in, { 0.8f, 0.5f, 0.3f, 1.0f }, { type, 0 });
        std::vector<float> tail (o.begin() + 4800, o.end());
        const double pk = stats (tail).peak;
        std::printf ("    drive type %d peak %.3f\n", type, pk);
        CHECK (pk > 0.25 && pk < 1.0);
    }
}

TEST ("shape: RING makes sum and difference tones, SHIFT shifts")
{
    const float sr = 48000.0f;
    const auto in = sine (1000.0f, sr, 48000, 0.5f);
    // RING with carrier ringFreqHz(v) = 200 Hz
    const float v = std::log10 (200.0f / 0.5f) / 4.0f;
    const auto out = runFx (ModuleType::Ring, in, { v, 1.0f, 0.5f, 0.0f }, { 0, 0 });
    CHECK (toneMagHann (out, 1200.0f, sr, 4800) > 0.1);
    CHECK (toneMagHann (out, 800.0f, sr, 4800) > 0.1);
    CHECK (toneMagHann (out, 1000.0f, sr, 4800) < 0.01);
    // SHIFT +200 Hz: bipolar b with b^2 * 2000 = 200
    const float b = std::sqrt (0.1f);
    const auto sh = runFx (ModuleType::Ring, in, { 0.5f + 0.5f * b, 1.0f, 0.5f, 0.0f }, { 1, 0 });
    const double up = toneMagHann (sh, 1200.0f, sr, 4800), down = toneMagHann (sh, 800.0f, sr, 4800);
    std::printf ("    shift up %.3f down %.4f\n", up, down);
    CHECK (up > 0.3 && down < up * 0.05);
}

TEST ("shape: COMB resonates at the tuned frequency")
{
    const float sr = 48000.0f;
    std::vector<float> in (48000, 0.0f);
    in[0] = 0.5f;
    const auto out = runFx (ModuleType::Comb, in, { 0.5f, 0.95f, 0.0f, 1.0f }, { 0, 0 });   // C3 = 261.6 Hz
    // the 10th echo of the impulse: peak position / 10 = period (parabolic refinement)
    const double period = sr / dsp::noteToHz (60.0f);
    const int lo = (int) (period * 10 - 20), hi = (int) (period * 10 + 20);
    int best = lo;
    for (int i = lo; i < hi; ++i) if (std::abs (out[(size_t) i]) > std::abs (out[(size_t) best])) best = i;
    const double ym = std::abs (out[(size_t) best - 1]), y0 = std::abs (out[(size_t) best]), yp = std::abs (out[(size_t) best + 1]);
    const double pos = best + 0.5 * (ym - yp) / (ym - 2 * y0 + yp);
    std::printf ("    comb period %.3f expected %.3f\n", pos / 10.0, period);
    CHECK_NEAR (pos / 10.0 / period, 1.0, 0.01);
    const auto o2 = runFx (ModuleType::Comb, noise (96000, 0.5f), { 0.5f, 1.0f, 0.0f, 1.0f }, { 0, 0 });
    CHECK (stats (o2).peak < 3.0);
}

TEST ("shape: FORMANT moves the spectral centroid across vowels")
{
    const float sr = 48000.0f;
    // saw at 110 Hz
    std::vector<float> saw (48000);
    double ph = 0.0;
    for (auto& v : saw) { v = (float) (ph * 2.0 - 1.0) * 0.4f; ph += 110.0 / sr; ph -= std::floor (ph); }
    auto centroid = [&] (const std::vector<float>& x)
    {
        double num = 0.0, den = 0.0;
        for (int h = 1; h <= 40; ++h) { const double m = toneMagHann (x, 110.0f * h, sr, 4800); num += m * h * 110.0; den += m; }
        return num / den;
    };
    const double a = centroid (runFx (ModuleType::Formant, saw, { 0.0f, 0.5f, 0.5f, 1.0f }, { 0, 0 }));
    const double i = centroid (runFx (ModuleType::Formant, saw, { 0.5f, 0.5f, 0.5f, 1.0f }, { 0, 0 }));
    const double u = centroid (runFx (ModuleType::Formant, saw, { 1.0f, 0.5f, 0.5f, 1.0f }, { 0, 0 }));
    std::printf ("    centroids A %.0f I %.0f U %.0f\n", a, i, u);
    CHECK (i > u * 1.4 && a > u * 1.2);
    const auto o = runFx (ModuleType::Formant, saw, { 0.0f, 0.5f, 0.5f, 1.0f }, { 0, 0 });
    std::printf ("    formant rms in %.3f out %.3f\n", stats (saw).rms, stats (o, 4800).rms);
    CHECK (stats (o, 4800).rms > stats (saw).rms * 0.25 && stats (o, 4800).rms < stats (saw).rms * 3.0);
}

TEST ("shape: benchmarks")
{
    for (auto t : { ModuleType::Filter, ModuleType::Fold, ModuleType::Drive, ModuleType::Ring, ModuleType::Comb, ModuleType::Formant })
        benchmark (t, 1);
    const int ring[2] = { 1, 0 };
    benchmark (ModuleType::Ring, 1, nullptr, ring);
}
