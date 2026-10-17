// The patch: everything a preset stores. Plain data, edited on the message thread only.
#pragma once

#include "Types.h"
#include "ModuleSpec.h"
#include <array>
#include <memory>
#include <string>
#include <vector>

namespace weft
{
struct AudioResource;

struct ModuleData
{
    ModuleType type = ModuleType::None;
    std::array<float, kNumParams> params { 0.5f, 0.5f, 0.5f, 0.5f };
    std::array<int, kNumModes> modes { 0, 0 };
    std::array<float, kAuxSize> aux {};
    std::shared_ptr<const AudioResource> resource;   // GRAIN sample, WAVE user table

    bool empty() const noexcept { return type == ModuleType::None; }
};

struct LaneData
{
    std::string name;                      // shown vertically next to the lane letter
    bool toMix = true;
    std::array<bool, kNumLanes> toLane {}; // send this lane's output into other lanes (incl. backwards = feedback)
    float level = 0.8f;
    bool mute = false, solo = false, freeze = false;
    LaneTrigger trigger = LaneTrigger::Notes;
};

struct Connection
{
    SourceRef src;
    DestRef dst;
    float depth = 0.3f;   // -1..1 in normalized parameter units
    int id = -1;          // runtime only: stable index into the engine's depth table (not saved)
};

struct MacroData
{
    std::string name;     // "BLOOM"; empty -> shown as "—"
    float value = 0.0f;
    int cc = -1;          // MIDI CC mapping, -1 = none
};

struct SceneData
{
    bool stored = false;
    std::array<float, kNumMorphable> values {};   // cells (16 x 4) then slots (8 x 4)
};

struct VoiceSettings
{
    int polyphony = 8;          // 1 = MONO
    float attack = 0.005f;      // seconds, amp envelope of voice lanes
    float release = 0.6f;
    float glide = 0.0f;         // seconds
    float spread = 0.3f;        // stereo spread of voices 0..1
    int bendRange = 2;          // semitones
};

struct Patch
{
    std::string name = "Init";
    std::string category = "INIT";
    std::string author;
    int number = 0;

    std::array<ModuleData, kNumCells> cells {};
    std::array<ModuleData, kNumSlots> slots {};
    std::array<LaneData, kNumLanes> lanes {};
    std::array<MacroData, kNumMacros> macros {};
    std::vector<Connection> connections;

    std::array<SceneData, kNumScenes> scenes {};
    int selectedScene = 0;
    float morphX = 0.0f, morphY = 0.0f;

    VoiceSettings voice;
    float master = 0.8f;   // normalized master level
    float chaos = 0.12f;   // mutation range

    // Matrix rows / columns pinned by the user (sources without connections stay visible).
    std::vector<SourceRef> pinnedSources;
    std::vector<DestRef> pinnedDests;

    Patch();
};

/** Set a cell or slot to a module type with default parameters, migrating values by meaning. */
void assignModule (ModuleData& m, ModuleType newType, bool migrate);

/** Remove connections that point at empty cells/slots or reference invalid indices; clamp depths. */
void sanitizeConnections (Patch&);

/** Index of an existing connection or -1. */
int findConnection (const Patch&, SourceRef, DestRef);

/** True when the source refers to something that exists in the patch. */
bool sourceExists (const Patch&, SourceRef);
bool destExists (const Patch&, DestRef);

/** Bit mask of lanes that send into `lane`. */
uint8_t inboundMask (const Patch&, int lane);

/** Gather / scatter the morphable values (cell + slot params). */
void captureMorphable (const Patch&, std::array<float, kNumMorphable>&);
void applyMorphable (Patch&, const std::array<float, kNumMorphable>&);

/** Scene weights for a morph position (bilinear: A top-left, B top-right, C bottom-left, D bottom-right). */
std::array<float, kNumScenes> morphWeights (float x, float y) noexcept;
/** The morph position of a scene's corner. */
void sceneCorner (int scene, float& x, float& y) noexcept;

/** A tidy empty patch: four lanes (VOICE, TEXTURE, SPACE, BED), one WAVE voice, one LFO and one ENV. */
Patch makeInitPatch();

} // namespace weft
