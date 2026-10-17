// Module interfaces. Every cell holds an AudioModule; every modulator slot holds a ControlModule.
//
// Contract for implementers (audio thread rules):
//   * prepare() may allocate; it runs on the message thread or in prepareToPlay, never concurrently
//     with process().
//   * reset(), noteOn(), noteOff(), process() must not allocate, lock, or block.
//   * process() works in place on 1 (per-voice) or 2 (global, stereo) channels of <= kBlock samples.
//   * Parameters arrive as per-sample normalized arrays in [0,1] (already smoothed and modulated).
//   * Output must stay finite and roughly within ±4; the engine sanitizes anyway, but a module that
//     relies on that is a bug.
#pragma once

#include "Types.h"
#include <memory>
#include <string>
#include <vector>

namespace weft
{
/** Per-voice note state (or the global "last note" state for non-voiced contexts). */
struct NoteState
{
    float pitch = 60.0f;     // MIDI note incl. glide and bend
    float velocity = 0.8f;   // 0..1
    bool gate = false;       // key held
    int trigger = -1;        // sample offset of a note-on inside this sub-block, -1 if none
    int release = -1;        // sample offset of a note-off inside this sub-block, -1 if none
};

struct Transport
{
    double bpm = 120.0;
    double ppq = 0.0;            // beat position at the start of this sub-block (runs freely when the host is stopped)
    bool playing = false;
    float samplesPerBeat = 24000.0f;
};

/** Immutable audio resource (sample or wavetable). Shared between instances, created off the audio thread. */
struct AudioResource
{
    std::string name;            // shown in the cell header, e.g. "rain_tin.wav" or "Glass Bell"
    float sampleRate = 48000.0f;
    int channels = 1;
    std::vector<float> data;     // interleaved
    int frames() const noexcept { return channels > 0 ? (int) (data.size() / (size_t) channels) : 0; }
    // Wavetables: `waveFrames` frames, each stored as `waveLevels` band-limited mip levels of
    // kWaveFrameSize mono samples. Level L contains harmonics 1 .. (kWaveFrameSize/2) >> L.
    // Sample (frame f, level L, index i) = data[((f * waveLevels) + L) * kWaveFrameSize + i].
    static constexpr int kWaveFrameSize = 2048;
    static constexpr int kWaveMipLevels = 11;
    bool isWavetable = false;
    int waveFrames = 0;
    int waveLevels = 0;
    int numWaveFrames() const noexcept { return isWavetable ? waveFrames : 0; }
    const float* waveFrame (int f, int level) const noexcept
    {
        return data.data() + ((size_t) f * (size_t) waveLevels + (size_t) level) * (size_t) kWaveFrameSize;
    }
};

struct ParamInputs
{
    const float* v[kNumParams] {};   // per-sample normalized values
    int mode[kNumModes] {};
};

struct ProcessContext
{
    int numSamples = 0;
    float sampleRate = 48000.0f;
    float invSampleRate = 1.0f / 48000.0f;
    int voice = -1;                          // -1 = global (stereo) instance
    const NoteState* note = nullptr;         // never null during process()
    const Transport* transport = nullptr;    // never null during process()
    const float* aux = nullptr;              // kAuxSize values, never null
    const AudioResource* resource = nullptr; // may be null

    // Lane inputs for the IN module: laneIn[lane][channel]; channel count = numChannels of this call.
    // Only lanes whose bit is set in inboundMask carry signal; others are null.
    const float* const* laneIn = nullptr;    // laneIn[lane * 2 + ch]
    uint8_t inboundMask = 0;

    // Side-chain input (INPUT module). Null when the host provides none.
    const float* sidechain[2] { nullptr, nullptr };
};

class AudioModule
{
public:
    virtual ~AudioModule() = default;
    virtual void prepare (double sampleRate) = 0;
    virtual void reset() = 0;
    virtual void noteOn (const NoteState&) {}
    virtual void noteOff() {}
    virtual void process (const ProcessContext&, const ParamInputs&, float* const* io, int numChannels) = 0;
};

struct ControlContext
{
    int numSamples = 0;
    float sampleRate = 48000.0f;
    int voice = -1;
    const NoteState* note = nullptr;
    const Transport* transport = nullptr;
    const float* aux = nullptr;
    const float* slotValues = nullptr;    // kNumSlots latest outputs (for LOGIC / MATH inputs)
    const float* macroValues = nullptr;   // kNumMacros
    const float* const* laneSignal = nullptr; // kNumLanes mono buffers of the latest lane outputs (FOLLOW)
    int laneSignalLength = 0;
    const float* sidechain = nullptr;     // mono side-chain buffer or null
};

class ControlModule
{
public:
    virtual ~ControlModule() = default;
    virtual void prepare (double sampleRate) = 0;
    virtual void reset() = 0;
    virtual void noteOn (const NoteState&) {}
    virtual void noteOff() {}
    /** Advance by ctx.numSamples and return the output in [0,1] at the end of the sub-block. */
    virtual float process (const ControlContext&, const ParamInputs&) = 0;
    /** Optional: normalized cycle position [0,1) for UI playheads, or -1. Called from the audio thread only. */
    virtual float phase() const { return -1.0f; }
};

// Factories (implemented in modules/*.cpp). Return null for types that are not of that kind.
std::unique_ptr<AudioModule> createAudioModule (ModuleType);
std::unique_ptr<ControlModule> createControlModule (ModuleType);

// Per-family factories, each implemented in its own file.
std::unique_ptr<AudioModule> createSourceModule (ModuleType);   // modules/Source.cpp (incl. IN)
std::unique_ptr<AudioModule> createShapeModule (ModuleType);    // modules/Shape.cpp
std::unique_ptr<AudioModule> createSpaceModule (ModuleType);    // modules/Space.cpp
std::unique_ptr<ControlModule> createControlModuleImpl (ModuleType); // modules/Control.cpp

} // namespace weft
