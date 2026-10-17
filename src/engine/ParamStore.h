// Lock-free shared state between the message thread (UI, host parameters) and the audio thread.
// Every continuous value is a relaxed atomic: writers never block, readers see the latest value.
#pragma once

#include "Types.h"
#include <array>
#include <atomic>

namespace weft
{
template <typename T, size_t N>
using AtomicArray = std::array<std::atomic<T>, N>;

struct ParamStore
{
    AtomicArray<float, kNumCells * kNumParams> cell {};
    AtomicArray<float, kNumSlots * kNumParams> slot {};
    AtomicArray<int,   kNumCells * kNumModes>  cellMode {};
    AtomicArray<int,   kNumSlots * kNumModes>  slotMode {};
    AtomicArray<float, kNumCells * kAuxSize>   cellAux {};
    AtomicArray<float, kNumSlots * kAuxSize>   slotAux {};
    AtomicArray<float, kNumMacros>             macro {};
    AtomicArray<float, kMaxConnections>        depth {};

    AtomicArray<float, kNumLanes> laneLevel {};
    AtomicArray<int,   kNumLanes> laneMute {};
    AtomicArray<int,   kNumLanes> laneSolo {};
    AtomicArray<int,   kNumLanes> laneFreeze {};

    std::atomic<float> master { 0.8f };
    std::atomic<float> chaos { 0.12f };
    std::atomic<float> morphX { 0.0f }, morphY { 0.0f };

    // Scenes (for morphing on the audio thread when the morph position is automated).
    std::array<AtomicArray<float, kNumMorphable>, kNumScenes> scene {};
    AtomicArray<int, kNumScenes> sceneStored {};
    std::atomic<uint32_t> sceneVersion { 0 };

    // Voice settings
    std::atomic<int>   polyphony { 8 };
    std::atomic<float> attack { 0.005f }, release { 0.6f }, glide { 0.0f }, spread { 0.3f };
    std::atomic<int>   bendRange { 2 };

    // MIDI CC -> macro map (-1 = none), and a "learn" request: macro index waiting for a CC, -1 if none.
    AtomicArray<int, kNumMacros> macroCC {};
    std::atomic<int> learnMacro { -1 };
    std::atomic<int> learnedCC { -1 };   // written by the audio thread when a learn completes

    // One-shot requests from the message thread.
    std::atomic<int> panicRequest { 0 };

    ParamStore()
    {
        for (auto& v : cell) v.store (0.5f, std::memory_order_relaxed);
        for (auto& v : slot) v.store (0.5f, std::memory_order_relaxed);
        for (auto& v : laneLevel) v.store (0.8f, std::memory_order_relaxed);
        for (int i = 0; i < kNumMacros; ++i) macroCC[(size_t) i].store (20 + i, std::memory_order_relaxed);
    }

    float cellParam (int c, int k) const noexcept { return cell[(size_t) (c * kNumParams + k)].load (std::memory_order_relaxed); }
    float slotParam (int s, int k) const noexcept { return slot[(size_t) (s * kNumParams + k)].load (std::memory_order_relaxed); }
    void setCellParam (int c, int k, float v) noexcept { cell[(size_t) (c * kNumParams + k)].store (v, std::memory_order_relaxed); }
    void setSlotParam (int s, int k, float v) noexcept { slot[(size_t) (s * kNumParams + k)].store (v, std::memory_order_relaxed); }

    /** Morphable index: cells first, then slots. */
    std::atomic<float>& morphable (int i) noexcept
    {
        return i < kNumCells * kNumParams ? cell[(size_t) i] : slot[(size_t) (i - kNumCells * kNumParams)];
    }
};

/** Single-producer / single-consumer ring of pointers (audio thread -> message thread garbage). */
template <typename T, int Capacity>
class SpscQueue
{
public:
    bool push (T v) noexcept
    {
        const auto w = write.load (std::memory_order_relaxed);
        const auto next = (w + 1) % Capacity;
        if (next == read.load (std::memory_order_acquire)) return false;
        items[(size_t) w] = v;
        write.store (next, std::memory_order_release);
        return true;
    }
    bool pop (T& v) noexcept
    {
        const auto r = read.load (std::memory_order_relaxed);
        if (r == write.load (std::memory_order_acquire)) return false;
        v = items[(size_t) r];
        read.store ((r + 1) % Capacity, std::memory_order_release);
        return true;
    }
    bool hasSpace() const noexcept
    {
        const auto w = write.load (std::memory_order_relaxed);
        return (w + 1) % Capacity != read.load (std::memory_order_acquire);
    }
private:
    std::array<T, Capacity> items {};
    std::atomic<int> read { 0 }, write { 0 };
};

} // namespace weft
