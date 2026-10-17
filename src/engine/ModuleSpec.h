// Static descriptions of every module: names, parameters, modes and how their values read.
// Used by the engine (defaults, migration, voicing rules), the host parameter layer and the UI.
#pragma once

#include "Types.h"
#include <string>
#include <vector>

namespace weft
{
/** What a parameter "means", used to carry values across when a cell's module is swapped. */
enum class Semantic : uint8_t
{
    None, Pitch, Frequency, Resonance, Amount, Mix, Level, Time, Feedback, Tone, Rate,
    Shape, Size, Position, Damping, Spread, Gain, Density, Probability, Smooth, Attack, Decay, Sustain, Release
};

enum class ModeStyle : uint8_t { Segmented, Toggle, Menu };

struct ModeSpec
{
    const char* name = "";
    std::vector<const char*> options;
    int defaultIndex = 0;
    ModeStyle style = ModeStyle::Segmented;
};

struct ParamSpec
{
    const char* label = "";
    float defaultValue = 0.5f;
    Semantic semantic = Semantic::None;
};

struct ModuleSpec
{
    ModuleType type = ModuleType::None;
    const char* name = "";             // "FILTER"
    Family family = Family::Utility;
    const char* blurb = "";            // one line for the module picker
    ParamSpec params[kNumParams];
    int numModes = 0;
    ModeSpec modes[kNumModes];
    bool polyCapable = false;          // may run once per voice
    bool noteSource = false;           // makes its lane a voice lane
    bool needsResource = false;        // sample / wavetable
};

const ModuleSpec& getSpec (ModuleType) noexcept;

inline bool isAudioType (ModuleType t) noexcept   { return t >= ModuleType::Osc && t <= ModuleType::Loop; }
inline bool isControlType (ModuleType t) noexcept { return t >= ModuleType::Lfo && t <= ModuleType::Physics; }

/** Module types offered for cells / modulator slots, in library order. */
const std::vector<ModuleType>& cellModuleTypes();
const std::vector<ModuleType>& controlModuleTypes();

struct LabelContext
{
    uint8_t inboundMask = 0;       // IN: lanes routed into this lane
    std::string resourceName;      // GRAIN / WAVE sample or table name
};

/** Parameter label for display, which may depend on the selected mode (e.g. FILTER BP: FREQ / Q). */
std::string paramLabel (ModuleType, int param, const int* modes);
/** Formatted value for display ("1.2k", "38%", "3/16", "−14 dB"...). */
std::string formatParam (ModuleType, int param, float value, const int* modes);
/** Short text after the module name in the cell header ("LP 24", "3/16 ping", "rain_tin.wav"). */
std::string subLabel (ModuleType, const float* params, const int* modes, const LabelContext&);
/** The two parameters shown on the cell (usually 0 and 1; IN shows its first two inbound lanes). */
void mainParams (ModuleType, const LabelContext&, int& first, int& second);

/** Note name in Logic's convention (60 = C3). */
std::string noteName (int midiNote);

/** Lane letter, cell id ("A3"), slot badge ("1"). */
inline char laneLetter (int lane) noexcept { return (char) ('A' + lane); }
std::string cellId (int cell);

} // namespace weft
