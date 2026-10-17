// The WEFT audio engine. One instance per plug-in instance.
//
//   Message thread: publish(Program), collectGarbage(), params() writes, telemetry() reads.
//   Audio thread:   process().
//   prepare() is called while audio is stopped.
#pragma once

#include "DspUtil.h"
#include "Module.h"
#include "ParamStore.h"
#include "Program.h"
#include "Telemetry.h"

#include <atomic>
#include <memory>

namespace weft
{
struct MidiEvent
{
    int offset = 0;       // sample offset in the host block
    uint8_t status = 0, data1 = 0, data2 = 0;
};

struct HostTransport
{
    bool valid = false;
    double bpm = 120.0;
    double ppq = 0.0;
    bool playing = false;
};

class Engine
{
public:
    Engine();
    ~Engine();

    void prepare (double sampleRate, int maxBlockSize);
    void releaseResources() {}

    /** Render `numSamples` into `out` (1 or 2 channels). `sidechain` may be null. */
    void process (float* const* out, int numOutChannels,
                  const float* const* sidechain, int numSidechainChannels,
                  int numSamples, const MidiEvent* events, int numEvents, const HostTransport&);

    /** Hand a new program to the audio thread (takes ownership). Message thread. */
    void publish (std::unique_ptr<Program>);
    /** Free programs the audio thread has retired. Message thread, call regularly. */
    void collectGarbage();

    ParamStore& params() noexcept { return store; }
    Telemetry& telemetry() noexcept { return tele; }
    double sampleRate() const noexcept { return rate; }

    /** Apply the morph position to the parameter store (shared by both threads). */
    static void applyMorph (ParamStore&, float x, float y);

private:
    struct Voice
    {
        bool active = false;      // envelope running
        bool gate = false;
        bool sustained = false;   // released while the sustain pedal was down
        int note = -1;
        float velocity = 0.0f;
        float pitch = 60.0f, targetPitch = 60.0f;
        float env = 0.0f;
        int envStage = 0;         // 0 idle, 1 attack, 2 sustain, 3 release
        float pressure = 0.0f;
        uint32_t age = 0;
        bool stealing = false;
        int stealNote = -1;
        float stealVelocity = 0.0f;
        float stealGain = 1.0f;
        NoteState state;
        float panL = 0.7071f, panR = 0.7071f;
        float envBuf[kBlock] {};
    };

    void renderSub (float* const* out, int numOut, const float* const* sidechain, int numSide, int offset, int n);
    void handleMidi (const MidiEvent&);
    void noteOn (int note, float velocity);
    void noteOff (int note);
    void startVoice (int v, int note, float velocity);
    void allNotesOff (bool hard);
    void panic();
    void updateVoices (int n);
    void evalSlots (int n);
    void evalLanes (const float* const* sidechain, int numSide, int n);
    void buildParams (const ParamRouting&, bool isSlot, int base, int voice, int n, float* out);
    const float* audioSource (int cell, int voice) const;
    float controlSource (const ModInput&, int voice, bool end) const;
    void writeScope (int cell, const float* x, int n);

    ParamStore store;
    Telemetry tele;

    std::atomic<Program*> pending { nullptr };
    Program* current = nullptr;
    SpscQueue<Program*, 512> retired;
    Program empty;

    double rate = 48000.0;
    float fRate = 48000.0f, invRate = 1.0f / 48000.0f;
    float smoothCoef = 1.0f, depthCoef = 1.0f;

    // Smoothed parameter state: previous (start of sub-block) and current (end) values.
    float cellPrev[kNumCells * kNumParams] {}, cellCur[kNumCells * kNumParams] {};
    float slotPrev[kNumSlots * kNumParams] {}, slotCur[kNumSlots * kNumParams] {};
    float macroPrev[kNumMacros] {}, macroCur[kNumMacros] {};
    float depthPrev[kMaxConnections] {}, depthCur[kMaxConnections] {};
    float laneGainPrev[kNumLanes] {}, laneGainCur[kNumLanes] {};
    float masterPrev = 0.0f, masterCur = 0.0f;
    const void* lastCellInst[kNumCells] {};
    const void* lastSlotInst[kNumSlots] {};
    bool firstBlock = true;

    // Modulator outputs
    float slotOutPrev[kNumSlots] {}, slotOut[kNumSlots] {};
    float slotVoicePrev[kNumSlots][kMaxVoices] {}, slotVoiceOut[kNumSlots][kMaxVoices] {};
    bool slotVoiceActive[kNumSlots] {};
    int traceCounter = 0;

    // MIDI state
    Voice voices[kMaxVoices];
    uint32_t ageCounter = 0;
    int noteStack[128] {};
    int noteStackSize = 0;
    bool sustainPedal = false;
    float bend = 0.0f;          // -1..1
    float wheelPrev = 0.0f, wheel = 0.0f, wheelTarget = 0.0f;
    float pressurePrev = 0.0f, pressure = 0.0f, pressureTarget = 0.0f;
    float lastVelocity = 0.8f;
    float lastPitch = 60.0f, lastTarget = 60.0f;
    bool anyTrigger = false;
    int heldCount = 0;
    NoteState globalNote;
    int newestVoice = -1;

    Transport transport;
    double freePpq = 0.0;

    // Buffers
    float laneOut[kNumLanes][2][kBlock] {};
    float laneOutPrev[kNumLanes][2][kBlock] {};
    float laneMono[kNumLanes][kBlock] {};
    float laneVoiceOut[kNumLanes][kMaxVoices][kBlock] {};
    float cellVoiceOut[kNumCells][kMaxVoices][kBlock] {};
    float cellMono[kNumCells][kBlock] {};
    float cellSum[kNumCells][kBlock] {};
    bool cellSumFresh[kNumCells] {};
    float scratchIn[kNumLanes][kBlock] {};
    float sideMono[kBlock] {};
    float zeros[kBlock] {};

    // Lane freeze processors and master stage
    std::unique_ptr<AudioModule> laneFreeze[kNumLanes];
    float freezeMix[kNumLanes] {};
    dsp::DcBlocker dcL, dcR;
    float meterPeak[kNumLanes] {};
    float masterPeak[2] {};
    float cellPeakState[kNumCells] {};
    float cpuSmoothed = 0.0f;
    std::atomic<float> lastMorphX { -1.0f }, lastMorphY { -1.0f };
    uint32_t lastSceneVersion = 0xffffffffu;
};

} // namespace weft
