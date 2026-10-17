// A Program is the compiled, immutable topology of a patch: which modules run where, per-voice or
// global, how lanes route, and which modulation sources feed each parameter. It is built on the
// message thread and handed to the audio thread through an atomic pointer. Module instances are
// shared between successive programs, so DSP state survives edits that do not touch that cell.
#pragma once

#include "Module.h"
#include "Patch.h"
#include <memory>
#include <mutex>
#include <vector>

namespace weft
{
constexpr int kMaxModsPerParam = 16;

struct ModInput
{
    SourceKind kind = SourceKind::Slot;
    uint8_t index = 0;
    uint8_t connId = 0;
};

struct ParamRouting
{
    uint8_t count = 0;        // control-rate inputs (slots, macros, midi)
    uint8_t audioCount = 0;   // audio-rate inputs (cell outputs), stored after the control ones
    ModInput in[kMaxModsPerParam] {};
};

struct CellInstances
{
    ModuleType type = ModuleType::None;
    std::unique_ptr<AudioModule> global;
    std::array<std::unique_ptr<AudioModule>, kMaxVoices> voice {};
    double preparedRate = 0.0;
    void prepare (double sr);
};

struct SlotInstances
{
    ModuleType type = ModuleType::None;
    std::unique_ptr<ControlModule> global;
    std::array<std::unique_ptr<ControlModule>, kMaxVoices> voice {};
    double preparedRate = 0.0;
    void prepare (double sr);
};

struct CellProg
{
    ModuleType type = ModuleType::None;
    CellInstances* inst = nullptr;
    bool voiced = false;            // runs once per voice
    bool audioSource = false;       // its output is used as an audio-rate modulation source
    uint8_t inboundMask = 0;        // IN module: lanes routed into this lane
    const AudioResource* resource = nullptr;
    ParamRouting routes[kNumParams] {};
};

struct LaneProg
{
    int cells[kCellsPerLane] { -1, -1, -1, -1 };   // non-empty cells in processing order
    int numCells = 0;
    int voicedCount = 0;      // leading cells processed per voice
    int entry = -1;           // position (within cells[]) of the first IN module, -1 = inputs enter at the lane start
    bool voiced = false;
    bool outputPerVoice = false;
    bool toMix = true;
    uint8_t toLaneMask = 0;
    uint8_t inboundMask = 0;
};

struct SlotProg
{
    ModuleType type = ModuleType::None;
    SlotInstances* inst = nullptr;
    ParamRouting routes[kNumParams] {};
};

struct Program
{
    CellProg cells[kNumCells] {};
    LaneProg lanes[kNumLanes] {};
    SlotProg slots[kNumSlots] {};
    bool anyVoicedLane = false;
    uint32_t serial = 0;

    // Ownership (released on the message thread when the program is retired)
    std::vector<std::shared_ptr<CellInstances>> keepCells;
    std::vector<std::shared_ptr<SlotInstances>> keepSlots;
    std::vector<std::shared_ptr<const AudioResource>> keepResources;
};

/** Holds the live module instances per cell / slot and builds Programs. Message thread only. */
class InstanceRegistry
{
public:
    void setSampleRate (double sr);   // prepares every instance at the new rate
    double sampleRate() const { return rate; }

    /** Builds a program for the patch. Creates instances for cells / slots whose type changed. */
    std::unique_ptr<Program> compile (const Patch&);

    /** Drop all instances (e.g. when loading a patch, so state from the old patch does not leak). */
    void clear();

private:
    std::mutex lock;
    double rate = 0.0;
    uint32_t serial = 0;
    std::array<std::shared_ptr<CellInstances>, kNumCells> cells {};
    std::array<std::shared_ptr<SlotInstances>, kNumSlots> slots {};
};

/** Rules shared with the UI. */
bool laneIsVoiced (const Patch&, int lane);
/** Number of leading cells in a lane that run per voice (0 when the lane is global). */
int laneVoicedCount (const Patch&, int lane);

} // namespace weft
