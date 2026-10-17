// Audio thread -> UI data (scopes, meters, modulator traces). Written with relaxed atomics by the
// audio thread, read by the UI at frame rate; tearing between values is harmless for display.
#pragma once

#include "Types.h"
#include <array>
#include <atomic>

namespace weft
{
struct Telemetry
{
    static constexpr int kScopeSize = 2048;    // per cell, mono
    static constexpr int kTraceSize = 256;     // per modulator slot, one value per sub-block * kTraceDecimation
    static constexpr int kTraceDecimation = 8;

    std::array<std::array<std::atomic<float>, kScopeSize>, kNumCells> scope {};
    std::array<std::atomic<uint32_t>, kNumCells> scopeWrite {};
    std::array<std::atomic<float>, kNumCells> cellPeak {};

    std::array<std::array<std::atomic<float>, kTraceSize>, kNumSlots> trace {};
    std::array<std::atomic<uint32_t>, kNumSlots> traceWrite {};
    std::array<std::atomic<float>, kNumSlots> slotValue {};
    std::array<std::atomic<float>, kNumSlots> slotPhase {};

    // Live (modulated) normalized parameter values, for knob indicators.
    std::array<std::atomic<float>, kNumCells * kNumParams> cellLive {};
    std::array<std::atomic<float>, kNumSlots * kNumParams> slotLive {};

    std::array<std::atomic<float>, kNumLanes> lanePeak {};
    std::atomic<float> masterPeakL { 0.0f }, masterPeakR { 0.0f };
    std::atomic<float> cpu { 0.0f };           // 0..1 of real time
    std::atomic<int> activeVoices { 0 };
    std::atomic<float> lastNote { 60.0f };
    std::atomic<float> bpm { 120.0f };
    std::atomic<int> hostPlaying { 0 };
    std::atomic<uint32_t> blocks { 0 };       // increments every processed host block
    std::atomic<float> sampleRate { 48000.0f };
};

} // namespace weft
