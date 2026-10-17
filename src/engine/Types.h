// WEFT engine — fundamental constants and identifiers.
// The engine is plain C++20 with no JUCE dependency so it can be unit-tested and
// profiled in isolation. Everything in this namespace that runs on the audio
// thread is allocation-free and lock-free.
#pragma once

#include <array>
#include <cstdint>

namespace weft
{
constexpr int kNumLanes      = 4;
constexpr int kCellsPerLane  = 4;
constexpr int kNumCells      = kNumLanes * kCellsPerLane;
constexpr int kNumSlots      = 8;     // modulator slots (the coloured sources)
constexpr int kNumMacros     = 8;
constexpr int kNumScenes     = 4;
constexpr int kNumParams     = 4;     // continuous parameters per module ("one knob = one function")
constexpr int kNumModes      = 2;     // discrete selectors per module (focus-panel segmented control)
constexpr int kAuxSize       = 16;    // per-module auxiliary data (sequencer steps, input selectors...)
constexpr int kMaxVoices     = 8;
constexpr int kBlock         = 32;    // internal sub-block length: control rate and feedback latency
constexpr int kMaxConnections = 128;
constexpr int kNumMidiSources = 4;    // VEL, KEY, WHEEL, PRESS
constexpr int kNumMorphable  = kNumCells * kNumParams + kNumSlots * kNumParams;

enum class Family : uint8_t { Source, Shape, Space, Control, Utility };

enum class ModuleType : uint8_t
{
    None = 0,
    // SOURCE
    Osc, Wave, Noise, Grain, Input, Pluck,
    // utility: lane input mixer ("IN ← A + B")
    In,
    // SHAPE
    Filter, Fold, Drive, Ring, Comb, Formant,
    // SPACE
    Delay, Verb, Shift, Freeze, Chorus, Loop,
    // CONTROL (modulators)
    Lfo, Env, Rand, Seq, Follow, Drift, Logic, Math, Clock, Physics,
    Count
};

constexpr int kNumModuleTypes = (int) ModuleType::Count;

enum class SourceKind : uint8_t { Slot = 0, Macro = 1, Audio = 2, Midi = 3 };
enum class DestKind   : uint8_t { Cell = 0, Slot = 1 };
enum class MidiSource : uint8_t { Velocity = 0, Key = 1, Wheel = 2, Pressure = 3 };

struct SourceRef
{
    SourceKind kind = SourceKind::Slot;
    uint8_t index = 0;   // slot 0..7, macro 0..7, cell 0..15, midi 0..3

    friend bool operator== (SourceRef a, SourceRef b) noexcept { return a.kind == b.kind && a.index == b.index; }
    friend bool operator!= (SourceRef a, SourceRef b) noexcept { return ! (a == b); }
};

struct DestRef
{
    DestKind kind = DestKind::Cell;
    uint8_t index = 0;   // cell 0..15 or slot 0..7
    uint8_t param = 0;   // 0..3

    friend bool operator== (DestRef a, DestRef b) noexcept { return a.kind == b.kind && a.index == b.index && a.param == b.param; }
    friend bool operator!= (DestRef a, DestRef b) noexcept { return ! (a == b); }
    int flatIndex() const noexcept { return kind == DestKind::Cell ? index * kNumParams + param
                                                                    : kNumCells * kNumParams + index * kNumParams + param; }
};

inline int laneOf (int cell) noexcept     { return cell / kCellsPerLane; }
inline int posInLane (int cell) noexcept  { return cell % kCellsPerLane; }
inline int cellIndex (int lane, int pos) noexcept { return lane * kCellsPerLane + pos; }

enum class LaneTrigger : uint8_t { Notes = 0, Free = 1 };

} // namespace weft
