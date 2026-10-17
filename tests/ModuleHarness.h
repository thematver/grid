// Helpers for driving modules outside the engine in tests.
#pragma once

#include "engine/DspUtil.h"
#include "engine/Module.h"
#include "engine/ModuleSpec.h"
#include "engine/Types.h"

#include <algorithm>
#include <cmath>
#include <memory>
#include <vector>

namespace wtest
{
struct Stats
{
    double rms = 0.0, peak = 0.0, mean = 0.0;
    bool finite = true;
    size_t n = 0;
};

inline Stats stats (const std::vector<float>& x, size_t skip = 0)
{
    Stats s;
    double sum = 0.0, sq = 0.0;
    for (size_t i = skip; i < x.size(); ++i)
    {
        const double v = x[i];
        if (! std::isfinite (v)) { s.finite = false; continue; }
        sum += v; sq += v * v; s.peak = std::max (s.peak, std::abs (v)); ++s.n;
    }
    if (s.n) { s.mean = sum / (double) s.n; s.rms = std::sqrt (sq / (double) s.n); }
    return s;
}

/** Runs an AudioModule with constant parameters over `input` (mono or stereo), returns output channels. */
struct AudioRun
{
    weft::ModuleType type = weft::ModuleType::None;
    float sampleRate = 48000.0f;
    int channels = 2;               // 1 = per-voice context, 2 = global stereo
    float params[weft::kNumParams] { 0.5f, 0.5f, 0.5f, 0.5f };
    int modes[weft::kNumModes] { 0, 0 };
    float aux[weft::kAuxSize] {};
    weft::NoteState note;
    weft::Transport transport;
    const weft::AudioResource* resource = nullptr;
    const float* laneIn[weft::kNumLanes * 2] {};
    uint8_t inboundMask = 0;

    AudioRun()
    {
        note.pitch = 60.0f; note.velocity = 0.8f; note.gate = true; note.trigger = 0;
        transport.bpm = 120.0; transport.samplesPerBeat = 24000.0f;
    }

    void useDefaults()
    {
        const auto& s = weft::getSpec (type);
        for (int k = 0; k < weft::kNumParams; ++k) params[k] = s.params[k].defaultValue;
        for (int m = 0; m < weft::kNumModes; ++m) modes[m] = m < s.numModes ? s.modes[m].defaultIndex : 0;
    }

    /** Process `numSamples`; `inL/inR` may be null for silence. Returns {L, R} (R empty when mono). */
    std::vector<std::vector<float>> run (weft::AudioModule& m, int numSamples, const float* inL = nullptr, const float* inR = nullptr, bool triggerNote = true)
    {
        std::vector<std::vector<float>> out ((size_t) channels, std::vector<float> ((size_t) numSamples, 0.0f));
        if (inL) std::copy (inL, inL + numSamples, out[0].begin());
        if (channels > 1 && (inR || inL)) std::copy (inR ? inR : inL, (inR ? inR : inL) + numSamples, out[1].begin());
        if (triggerNote) m.noteOn (note);
        float pv[weft::kNumParams][weft::kBlock];
        for (int pos = 0; pos < numSamples; pos += weft::kBlock)
        {
            const int n = std::min (weft::kBlock, numSamples - pos);
            weft::ParamInputs pi;
            for (int k = 0; k < weft::kNumParams; ++k) { for (int i = 0; i < n; ++i) pv[k][i] = params[k]; pi.v[k] = pv[k]; }
            for (int i = 0; i < weft::kNumModes; ++i) pi.mode[i] = modes[i];
            weft::ProcessContext cx;
            cx.numSamples = n; cx.sampleRate = sampleRate; cx.invSampleRate = 1.0f / sampleRate;
            cx.voice = channels == 1 ? 0 : -1;
            weft::NoteState ns = note; ns.trigger = (pos == 0 && triggerNote) ? 0 : -1;
            cx.note = &ns; cx.transport = &transport; cx.aux = aux; cx.resource = resource;
            const float* li[weft::kNumLanes * 2] {};
            for (int l = 0; l < weft::kNumLanes * 2; ++l) li[l] = laneIn[l] ? laneIn[l] + pos : nullptr;
            cx.laneIn = li; cx.inboundMask = inboundMask;
            float* io[2] = { out[0].data() + pos, channels > 1 ? out[1].data() + pos : nullptr };
            m.process (cx, pi, io, channels);
            transport.ppq += (double) n / transport.samplesPerBeat;
        }
        return out;
    }
};

/** Runs a ControlModule with constant parameters; returns one output value per sub-block. */
struct ControlRun
{
    weft::ModuleType type = weft::ModuleType::None;
    float sampleRate = 48000.0f;
    float params[weft::kNumParams] { 0.5f, 0.5f, 0.5f, 0.5f };
    int modes[weft::kNumModes] { 0, 0 };
    float aux[weft::kAuxSize] {};
    float slotValues[weft::kNumSlots] {};
    float macroValues[weft::kNumMacros] {};
    weft::NoteState note;
    weft::Transport transport;
    std::vector<float> laneSignal[weft::kNumLanes];   // optional, per-sample lane audio (FOLLOW)

    ControlRun() { note.gate = true; transport.bpm = 120.0; transport.samplesPerBeat = 24000.0f; }

    void useDefaults()
    {
        const auto& s = weft::getSpec (type);
        for (int k = 0; k < weft::kNumParams; ++k) params[k] = s.params[k].defaultValue;
        for (int m = 0; m < weft::kNumModes; ++m) modes[m] = m < s.numModes ? s.modes[m].defaultIndex : 0;
    }

    std::vector<float> run (weft::ControlModule& m, int numSamples, bool triggerNote = true, int releaseAtSample = -1)
    {
        std::vector<float> out;
        if (triggerNote) { note.gate = true; m.noteOn (note); }
        float pv[weft::kNumParams][weft::kBlock];
        float laneBuf[weft::kNumLanes][weft::kBlock] {};
        const float* lanePtrs[weft::kNumLanes] = { laneBuf[0], laneBuf[1], laneBuf[2], laneBuf[3] };
        for (int pos = 0; pos < numSamples; pos += weft::kBlock)
        {
            const int n = std::min (weft::kBlock, numSamples - pos);
            if (releaseAtSample >= 0 && pos >= releaseAtSample && note.gate) { note.gate = false; m.noteOff(); }
            weft::ParamInputs pi;
            for (int k = 0; k < weft::kNumParams; ++k) { for (int i = 0; i < n; ++i) pv[k][i] = params[k]; pi.v[k] = pv[k]; }
            for (int i = 0; i < weft::kNumModes; ++i) pi.mode[i] = modes[i];
            for (int l = 0; l < weft::kNumLanes; ++l)
                for (int i = 0; i < n; ++i) laneBuf[l][i] = (size_t) (pos + i) < laneSignal[l].size() ? laneSignal[l][(size_t) (pos + i)] : 0.0f;
            weft::ControlContext cx;
            cx.numSamples = n; cx.sampleRate = sampleRate; cx.voice = -1;
            weft::NoteState ns = note; ns.trigger = (pos == 0 && triggerNote) ? 0 : -1;
            cx.note = &ns; cx.transport = &transport; cx.aux = aux;
            cx.slotValues = slotValues; cx.macroValues = macroValues;
            cx.laneSignal = lanePtrs; cx.laneSignalLength = n;
            out.push_back (m.process (cx, pi));
            transport.ppq += (double) n / transport.samplesPerBeat;
        }
        return out;
    }
};

inline std::vector<float> sine (float hz, float sr, int n, float amp = 0.5f)
{
    std::vector<float> x ((size_t) n);
    for (int i = 0; i < n; ++i) x[(size_t) i] = amp * std::sin (2.0f * 3.14159265f * hz * (float) i / sr);
    return x;
}

inline std::vector<float> noise (int n, float amp = 0.5f, uint32_t seed = 1234)
{
    weft::dsp::Rng r (seed);
    std::vector<float> x ((size_t) n);
    for (auto& v : x) v = amp * r.bipolar();
    return x;
}

/** Estimated fundamental via zero crossings (for sanity checks of oscillators). */
inline double zeroCrossingHz (const std::vector<float>& x, float sr, size_t skip = 0)
{
    int crossings = 0;
    for (size_t i = skip + 1; i < x.size(); ++i) if (x[i - 1] < 0.0f && x[i] >= 0.0f) ++crossings;
    const double secs = (double) (x.size() - skip) / sr;
    return secs > 0 ? crossings / secs : 0.0;
}
} // namespace wtest
