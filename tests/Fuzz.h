// Robustness helpers shared by the module family tests: every mode combination, several sample
// rates, mono and stereo, constant extremes and random per-sample modulation.
#pragma once

#include "ModuleHarness.h"
#include "engine/Builtins.h"

#include <array>
#include <chrono>
#include <cstdio>

namespace wtest
{
struct FuzzResult { bool finite = true; double peak = 0.0; };

/** Runs `type` with per-sample random-walk parameters (or constants when `walk` is false). */
inline FuzzResult fuzzRun (weft::ModuleType type, float sr, int channels, const int* modes, const float* constParams,
                           bool walk, uint32_t seed, int numSamples, const weft::AudioResource* res = nullptr)
{
    using namespace weft;
    FuzzResult fr;
    auto m = createAudioModule (type);
    if (! m) return fr;
    m->prepare (sr);
    m->reset();
    dsp::Rng rng (seed);
    std::vector<float> in = noise (numSamples, 0.4f, seed ^ 0x55u);
    for (int i = 0; i < numSamples; ++i) in[(size_t) i] += 0.3f * std::sin (0.031f * (float) i);
    std::vector<float> L (in), R (in);
    float walkV[kNumParams];
    for (int k = 0; k < kNumParams; ++k) walkV[k] = constParams ? constParams[k] : rng.uniform();
    float pv[kNumParams][kBlock];
    float aux[kAuxSize];
    for (auto& a : aux) a = rng.uniform();
    NoteState note; note.pitch = 30.0f + 70.0f * rng.uniform(); note.gate = true; note.velocity = 0.9f;
    Transport tr; tr.samplesPerBeat = sr * 0.5f; tr.playing = (seed & 1) != 0;
    float laneBuf[kNumLanes * 2][kBlock];
    m->noteOn (note);
    for (int pos = 0; pos < numSamples; pos += kBlock)
    {
        const int n = std::min (kBlock, numSamples - pos);
        ParamInputs pi;
        for (int k = 0; k < kNumParams; ++k)
        {
            for (int i = 0; i < n; ++i)
            {
                if (walk)
                {
                    walkV[k] += 0.02f * rng.bipolar();
                    if (rng.uniform() < 0.002f) walkV[k] = rng.uniform();   // occasional jumps
                    walkV[k] = dsp::clamp01 (walkV[k]);
                }
                pv[k][i] = walkV[k];
            }
            pi.v[k] = pv[k];
        }
        for (int i = 0; i < kNumModes; ++i) pi.mode[i] = modes[i];
        for (int l = 0; l < kNumLanes * 2; ++l) for (int i = 0; i < n; ++i) laneBuf[l][i] = in[(size_t) (pos + i)] * (0.5f + 0.1f * l);
        const float* li[kNumLanes * 2];
        for (int l = 0; l < kNumLanes * 2; ++l) li[l] = laneBuf[l];
        ProcessContext cx;
        cx.numSamples = n; cx.sampleRate = sr; cx.invSampleRate = 1.0f / sr;
        cx.voice = channels == 1 ? 0 : -1;
        NoteState ns = note; ns.trigger = pos == 0 ? 0 : -1;
        if (walk && rng.uniform() < 0.01f) { note.gate = ! note.gate; if (note.gate) m->noteOn (note); else m->noteOff(); }
        note.pitch = dsp::clampf (note.pitch + 0.3f * rng.bipolar(), 0.0f, 127.0f);
        cx.note = &ns; cx.transport = &tr; cx.aux = aux; cx.resource = res;
        cx.laneIn = li; cx.inboundMask = 0x0F;
        cx.sidechain[0] = in.data() + pos; cx.sidechain[1] = in.data() + pos;
        float* io[2] = { L.data() + pos, channels > 1 ? R.data() + pos : nullptr };
        m->process (cx, pi, io, channels);
        tr.ppq += (double) n / tr.samplesPerBeat;
        for (int i = 0; i < n; ++i)
        {
            const float a = L[(size_t) (pos + i)], b = channels > 1 ? R[(size_t) (pos + i)] : 0.0f;
            if (! std::isfinite (a) || ! std::isfinite (b)) fr.finite = false;
            fr.peak = std::max (fr.peak, (double) std::max (std::abs (a), std::abs (b)));
        }
        if (! fr.finite) break;
    }
    return fr;
}

/** All mode combinations of a module type. */
inline std::vector<std::array<int, weft::kNumModes>> modeCombos (weft::ModuleType t)
{
    const auto& s = weft::getSpec (t);
    std::vector<std::array<int, weft::kNumModes>> out;
    const int n0 = s.numModes > 0 ? (int) s.modes[0].options.size() : 1;
    const int n1 = s.numModes > 1 ? (int) s.modes[1].options.size() : 1;
    for (int a = 0; a < n0; ++a) for (int b = 0; b < n1; ++b) out.push_back ({ a, b });
    return out;
}

/** The standard robustness sweep: returns the worst peak; reports any non-finite run. */
inline double robustnessSweep (weft::ModuleType type, int trials, double bound, const weft::AudioResource* res = nullptr)
{
    const float rates[] = { 44100.0f, 48000.0f, 96000.0f, 192000.0f };
    const float consts[3] = { 0.0f, 0.5f, 1.0f };
    double worst = 0.0;
    uint32_t seed = 1;
    for (const auto& modes : modeCombos (type))
        for (int ch = 1; ch <= 2; ++ch)
            for (float sr : rates)
            {
                for (float c : consts)
                {
                    const float p[4] = { c, c, c, c };
                    const auto r = fuzzRun (type, sr, ch, modes.data(), p, false, seed++, (int) (sr * 0.25f), res);
                    if (! r.finite || r.peak > bound)
                        std::printf ("    %s modes %d/%d ch %d sr %.0f const %.1f: finite %d peak %.2f\n",
                                     weft::getSpec (type).name, modes[0], modes[1], ch, sr, c, (int) r.finite, r.peak);
                    CHECK (r.finite);
                    worst = std::max (worst, r.peak);
                }
                for (int t = 0; t < trials; ++t)
                {
                    const auto r = fuzzRun (type, sr, ch, modes.data(), nullptr, true, seed++, 6000, res);
                    if (! r.finite || r.peak > bound)
                        std::printf ("    %s modes %d/%d ch %d sr %.0f trial %d: finite %d peak %.2f\n",
                                     weft::getSpec (type).name, modes[0], modes[1], ch, sr, t, (int) r.finite, r.peak);
                    CHECK (r.finite);
                    worst = std::max (worst, r.peak);
                }
            }
    CHECK_MSG (worst <= bound, std::string (weft::getSpec (type).name) + " peak " + std::to_string (worst));
    return worst;
}

/** ns per sample for one module (prints, never fails). */
inline double benchmark (weft::ModuleType type, int channels, const weft::AudioResource* res = nullptr, const int* modes = nullptr)
{
    using namespace weft;
    auto m = createAudioModule (type);
    const float sr = 48000.0f;
    m->prepare (sr);
    AudioRun run; run.type = type; run.channels = channels; run.resource = res; run.useDefaults();
    if (modes) for (int i = 0; i < kNumModes; ++i) run.modes[i] = modes[i];
    const int n = 48000 * 2;
    std::vector<float> in = noise (n, 0.3f);
    run.run (*m, 4800, in.data(), in.data());
    const auto t0 = std::chrono::steady_clock::now();
    run.run (*m, n, in.data(), in.data(), false);
    const auto t1 = std::chrono::steady_clock::now();
    const double ns = std::chrono::duration<double, std::nano> (t1 - t0).count() / n;
    std::printf ("    bench %-8s %s %6.1f ns/sample\n", getSpec (type).name, channels == 1 ? "mono  " : "stereo", ns);
    return ns;
}

/** Magnitude of the DFT of x at frequency hz (Goertzel). */
inline double toneMag (const std::vector<float>& x, float hz, float sr, size_t skip = 0)
{
    const double w = 2.0 * 3.14159265358979 * hz / sr;
    const double c = 2.0 * std::cos (w);
    double s1 = 0.0, s2 = 0.0;
    for (size_t i = skip; i < x.size(); ++i) { const double s0 = x[i] + c * s1 - s2; s2 = s1; s1 = s0; }
    const double re = s1 - s2 * std::cos (w), im = s2 * std::sin (w);
    return std::sqrt (re * re + im * im) * 2.0 / (double) (x.size() - skip);
}

/** Hann-windowed Goertzel magnitude (low leakage, for spectral purity checks). */
inline double toneMagHann (const std::vector<float>& x, float hz, float sr, size_t skip = 0)
{
    const double w = 2.0 * 3.14159265358979 * hz / sr;
    const double c = 2.0 * std::cos (w);
    const size_t n = x.size() - skip;
    double s1 = 0.0, s2 = 0.0;
    for (size_t i = 0; i < n; ++i)
    {
        const double win = 0.5 - 0.5 * std::cos (2.0 * 3.14159265358979 * (double) i / (double) (n - 1));
        const double s0 = x[skip + i] * win + c * s1 - s2; s2 = s1; s1 = s0;
    }
    const double re = s1 - s2 * std::cos (w), im = s2 * std::sin (w);
    return std::sqrt (re * re + im * im) * 4.0 / (double) n;
}

/** Dominant frequency by scanning Goertzel magnitudes (coarse then fine). */
inline double dominantHz (const std::vector<float>& x, float sr, float lo, float hi, size_t skip = 0)
{
    double best = 0.0, bestHz = lo;
    for (float f = lo; f <= hi; f *= 1.01f) { const double m = toneMag (x, f, sr, skip); if (m > best) { best = m; bestHz = f; } }
    const double c = bestHz;
    for (double f = c * 0.99; f <= c * 1.01; f += c * 0.0005) { const double m = toneMag (x, (float) f, sr, skip); if (m > best) { best = m; bestHz = f; } }
    return bestHz;
}
} // namespace wtest
