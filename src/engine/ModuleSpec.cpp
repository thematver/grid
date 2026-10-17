#include "ModuleSpec.h"
#include "ParamMaps.h"
#include "DspUtil.h"

#include <array>
#include <cmath>
#include <cstdio>

namespace weft
{
namespace
{
using S = Semantic;
using MS = ModeStyle;

ModuleSpec make (ModuleType t, const char* name, Family f, const char* blurb,
                 std::array<ParamSpec, 4> p, std::vector<ModeSpec> modes,
                 bool poly, bool noteSource, bool resource = false)
{
    ModuleSpec s;
    s.type = t; s.name = name; s.family = f; s.blurb = blurb;
    for (int i = 0; i < 4; ++i) s.params[i] = p[(size_t) i];
    s.numModes = (int) modes.size();
    for (int i = 0; i < s.numModes && i < kNumModes; ++i) s.modes[i] = modes[(size_t) i];
    s.polyCapable = poly; s.noteSource = noteSource; s.needsResource = resource;
    return s;
}

std::array<ModuleSpec, kNumModuleTypes> buildSpecs()
{
    std::array<ModuleSpec, kNumModuleTypes> a {};
    auto set = [&a] (ModuleSpec s) { a[(size_t) s.type] = std::move (s); };

    a[0].type = ModuleType::None; a[0].name = "";

    // ── SOURCE ────────────────────────────────────────────────────────────────────────────────
    set (make (ModuleType::Osc, "OSC", Family::Source, "Saw, square, sine, triangle · sync · FM input",
        {{ { "PITCH", 0.5f, S::Pitch }, { "SHAPE", 0.0f, S::Shape }, { "DETUNE", 0.0f, S::Spread }, { "FM", 0.0f, S::Amount } }},
        { { "WAVE", { "SAW", "SQR", "SIN", "TRI" }, 0, MS::Segmented }, { "SYNC", { "FREE", "SYNC" }, 0, MS::Toggle } },
        true, true));
    set (make (ModuleType::Wave, "WAVE", Family::Source, "Wavetables, or your own from any sample",
        {{ { "POS", 0.0f, S::Position }, { "PITCH", 0.5f, S::Pitch }, { "WARP", 0.5f, S::Shape }, { "FM", 0.0f, S::Amount } }},
        { { "TABLE", { "Glass Bell", "Vowels", "Harmonic", "Digital", "Metal", "Breath" }, 0, MS::Menu } },
        true, true, true));
    set (make (ModuleType::Noise, "NOISE", Family::Source, "White to brown, crackle, dust",
        {{ { "COLOR", 0.3f, S::Tone }, { "LEVEL", 0.6f, S::Level }, { "DUST", 0.0f, S::Density }, { "CRUSH", 0.0f, S::Amount } }},
        {}, true, true));
    set (make (ModuleType::Grain, "GRAIN", Family::Source, "Granular player: drop a sample on the cell",
        {{ { "POS", 0.3f, S::Position }, { "SIZE", 0.3f, S::Size }, { "DENS", 0.5f, S::Density }, { "JITTER", 0.2f, S::Spread } }},
        { { "DIR", { "FWD", "REV", "BOTH" }, 0, MS::Segmented }, { "KEY", { "FREE", "KEY" }, 1, MS::Toggle } },
        true, true, true));
    set (make (ModuleType::Input, "INPUT", Family::Source, "External audio or DAW side-chain",
        {{ { "GAIN", 0.5f, S::Gain }, { "GATE", 0.0f, S::Amount }, { "TONE", 0.5f, S::Tone }, { "WIDTH", 1.0f, S::Spread } }},
        {}, false, false));
    set (make (ModuleType::Pluck, "PLUCK", Family::Source, "Physical model of a string and resonator",
        {{ { "PITCH", 0.5f, S::Pitch }, { "DECAY", 0.45f, S::Decay }, { "TONE", 0.6f, S::Tone }, { "BODY", 0.3f, S::Amount } }},
        { { "EXCITE", { "PLUCK", "BOW", "INPUT" }, 0, MS::Segmented } },
        true, true));
    set (make (ModuleType::In, "IN", Family::Utility, "Lane input: mix the lanes routed here",
        {{ { "A", 1.0f, S::Level }, { "B", 1.0f, S::Level }, { "C", 1.0f, S::Level }, { "D", 1.0f, S::Level } }},
        {}, true, false));

    // ── SHAPE ─────────────────────────────────────────────────────────────────────────────────
    set (make (ModuleType::Filter, "FILTER", Family::Shape, "LP · BP · HP · NOTCH, 12/24 dB, self-oscillation",
        {{ { "CUT", 0.6f, S::Frequency }, { "RES", 0.2f, S::Resonance }, { "DRIVE", 0.0f, S::Gain }, { "KEY", 0.5f, S::Amount } }},
        { { "TYPE", { "LP", "BP", "HP", "NT" }, 0, MS::Segmented }, { "SLOPE", { "12dB", "24dB" }, 1, MS::Toggle } },
        true, false));
    set (make (ModuleType::Fold, "FOLD", Family::Shape, "West-coast wavefolder",
        {{ { "FOLD", 0.35f, S::Amount }, { "SYM", 0.5f, S::Shape }, { "TONE", 1.0f, S::Tone }, { "MIX", 1.0f, S::Mix } }},
        { { "STAGES", { "1", "2", "3", "4" }, 1, MS::Segmented } },
        true, false));
    set (make (ModuleType::Drive, "DRIVE", Family::Shape, "Tape, tube, diode, bitcrush",
        {{ { "GAIN", 0.4f, S::Gain }, { "TONE", 0.6f, S::Tone }, { "BIAS", 0.0f, S::Shape }, { "MIX", 1.0f, S::Mix } }},
        { { "TYPE", { "TAPE", "TUBE", "DIODE", "CRUSH" }, 0, MS::Segmented } },
        true, false));
    set (make (ModuleType::Ring, "RING", Family::Shape, "Ring modulator, frequency shifter",
        {{ { "FREQ", 0.5f, S::Frequency }, { "MIX", 1.0f, S::Mix }, { "FINE", 0.5f, S::Pitch }, { "SPREAD", 0.0f, S::Spread } }},
        { { "TYPE", { "RING", "SHIFT" }, 0, MS::Segmented }, { "KEY", { "FREE", "KEY" }, 0, MS::Toggle } },
        true, false));
    set (make (ModuleType::Comb, "COMB", Family::Shape, "Comb filter, tuned by notes",
        {{ { "TUNE", 0.5f, S::Pitch }, { "FB", 0.7f, S::Feedback }, { "DAMP", 0.3f, S::Damping }, { "MIX", 1.0f, S::Mix } }},
        { { "TUNING", { "TUNED", "FREE", "KEY" }, 0, MS::Segmented }, { "POLARITY", { "+FB", "−FB" }, 0, MS::Toggle } },
        true, false));
    set (make (ModuleType::Formant, "FORMANT", Family::Shape, "Vowels: morph A–E–I–O–U",
        {{ { "VOWEL", 0.0f, S::Position }, { "RES", 0.5f, S::Resonance }, { "SHIFT", 0.5f, S::Pitch }, { "MIX", 1.0f, S::Mix } }},
        { { "VOICE", { "LOW", "HIGH" }, 0, MS::Toggle } },
        true, false));

    // ── SPACE ─────────────────────────────────────────────────────────────────────────────────
    set (make (ModuleType::Delay, "DELAY", Family::Space, "Ping-pong, tape, wobble, feedback to ∞",
        {{ { "TIME", 0.45f, S::Time }, { "FB", 0.45f, S::Feedback }, { "TONE", 0.5f, S::Tone }, { "MIX", 0.35f, S::Mix } }},
        { { "TYPE", { "PING", "STEREO", "TAPE" }, 0, MS::Segmented }, { "SYNC", { "FREE", "SYNC" }, 1, MS::Toggle } },
        false, false));
    set (make (ModuleType::Verb, "VERB", Family::Space, "From a small room to an endless hall",
        {{ { "SIZE", 0.6f, S::Size }, { "DAMP", 0.4f, S::Damping }, { "PRE", 0.1f, S::Time }, { "MIX", 0.3f, S::Mix } }},
        { { "TYPE", { "ROOM", "HALL", "CLOUD" }, 1, MS::Segmented } },
        false, false));
    set (make (ModuleType::Shift, "SHIFT", Family::Space, "Pitch shift and spectral smear",
        {{ { "PITCH", 0.75f, S::Pitch }, { "BLUR", 0.2f, S::Amount }, { "FB", 0.0f, S::Feedback }, { "MIX", 0.5f, S::Mix } }},
        { { "TYPE", { "GRAIN", "SMEAR" }, 0, MS::Segmented }, { "QUANT", { "FREE", "SEMI" }, 1, MS::Toggle } },
        false, false));
    set (make (ModuleType::Freeze, "FREEZE", Family::Space, "Frozen spectrum, the base of textures",
        {{ { "HOLD", 0.0f, S::Amount }, { "FADE", 0.5f, S::Time }, { "SIZE", 0.6f, S::Size }, { "MIX", 1.0f, S::Mix } }},
        {}, false, false));
    set (make (ModuleType::Chorus, "CHORUS", Family::Space, "Chorus, flanger, ensemble",
        {{ { "RATE", 0.35f, S::Rate }, { "DEPTH", 0.5f, S::Amount }, { "FB", 0.0f, S::Feedback }, { "MIX", 0.5f, S::Mix } }},
        { { "TYPE", { "CHORUS", "FLANGE", "ENSEMBLE" }, 0, MS::Segmented } },
        false, false));
    set (make (ModuleType::Loop, "LOOP", Family::Space, "Looper with reverse and speed",
        {{ { "SPEED", 0.75f, S::Rate }, { "LENGTH", 0.5f, S::Time }, { "FEED", 0.9f, S::Feedback }, { "MIX", 0.5f, S::Mix } }},
        { { "STATE", { "REC", "PLAY" }, 0, MS::Segmented }, { "SYNC", { "FREE", "SYNC" }, 0, MS::Toggle } },
        false, false));

    // ── CONTROL ───────────────────────────────────────────────────────────────────────────────
    set (make (ModuleType::Lfo, "LFO", Family::Control, "Tempo sync, shape morph, per-voice phase",
        {{ { "RATE", 0.4f, S::Rate }, { "SHAPE", 0.5f, S::Shape }, { "PHASE", 0.0f, S::Position }, { "FADE", 0.0f, S::Attack } }},
        { { "WAVE", { "TRI", "SIN", "SAW", "SQR" }, 0, MS::Segmented }, { "TIMING", { "FREE", "SYNC", "KEY" }, 0, MS::Segmented } },
        false, false));
    set (make (ModuleType::Env, "ENV", Family::Control, "ADSR and looping envelopes",
        {{ { "A", 0.21f, S::Attack }, { "D", 0.716f, S::Decay }, { "S", 0.0f, S::Sustain }, { "R", 0.6f, S::Release } }},
        { { "TYPE", { "ADSR", "AD", "LOOP" }, 0, MS::Segmented }, { "CURVE", { "EXP", "LIN" }, 0, MS::Toggle } },
        false, false));
    set (make (ModuleType::Rand, "RAND", Family::Control, "S&H, smooth random, probability",
        {{ { "RATE", 6.0f / 19.0f, S::Rate }, { "SMOOTH", 0.0f, S::Smooth }, { "PROB", 1.0f, S::Probability }, { "RANGE", 1.0f, S::Amount } }},
        { { "TYPE", { "S&H", "WALK" }, 0, MS::Segmented }, { "SYNC", { "FREE", "SYNC" }, 1, MS::Toggle } },
        false, false));
    set (make (ModuleType::Seq, "SEQ", Family::Control, "Step sequencer with step probability",
        {{ { "RATE", 3.0f / 19.0f, S::Rate }, { "STEPS", 1.0f, S::Amount }, { "GLIDE", 0.0f, S::Smooth }, { "PROB", 1.0f, S::Probability } }},
        { { "DIR", { "FWD", "REV", "PING", "RAND" }, 0, MS::Segmented }, { "SYNC", { "FREE", "SYNC" }, 1, MS::Toggle } },
        false, false));
    set (make (ModuleType::Follow, "FOLLOW", Family::Control, "Envelope of any lane's level",
        {{ { "ATK", 0.35f, S::Attack }, { "REL", 0.5f, S::Release }, { "GAIN", 0.333f, S::Gain }, { "FLOOR", 0.0f, S::Amount } }},
        { { "SOURCE", { "A", "B", "C", "D", "IN" }, 1, MS::Segmented }, { "DETECT", { "PEAK", "RMS" }, 1, MS::Toggle } },
        false, false));
    set (make (ModuleType::Drift, "DRIFT", Family::Control, "Slow analogue drift",
        {{ { "RATE", 0.5f, S::Rate }, { "DEPTH", 1.0f, S::Amount }, { "WANDER", 0.5f, S::Shape }, { "BIAS", 0.5f, S::Position } }},
        { { "SPEED", { "SLOW", "MED", "FAST" }, 0, MS::Segmented } },
        false, false));
    set (make (ModuleType::Logic, "LOGIC", Family::Control, "AND · OR · XOR, comparator, triggers",
        {{ { "THRESH", 0.5f, S::Amount }, { "SLEW", 0.0f, S::Smooth }, { "PROB", 1.0f, S::Probability }, { "HOLD", 0.0f, S::Time } }},
        { { "OP", { "AND", "OR", "XOR", ">" }, 0, MS::Segmented } },
        false, false));
    set (make (ModuleType::Math, "MATH", Family::Control, "Sum, min/max, quantizer, slew",
        {{ { "A", 1.0f, S::Amount }, { "B", 1.0f, S::Amount }, { "QUANT", 0.0f, S::Amount }, { "SLEW", 0.0f, S::Smooth } }},
        { { "OP", { "SUM", "MIN", "MAX", "MUL", "DIFF" }, 0, MS::Segmented } },
        false, false));
    set (make (ModuleType::Clock, "CLOCK", Family::Control, "Divider, euclidean rhythms, swing",
        {{ { "RATE", 3.0f / 19.0f, S::Rate }, { "STEPS", 7.0f / 15.0f, S::Amount }, { "FILL", 0.625f, S::Density }, { "SWING", 0.0f, S::Shape } }},
        { { "TYPE", { "DIV", "EUCLID" }, 1, MS::Segmented }, { "SYNC", { "FREE", "SYNC" }, 1, MS::Toggle } },
        false, false));
    set (make (ModuleType::Physics, "PHYSICS", Family::Control, "Bouncing ball, pendulum, gravity",
        {{ { "GRAVITY", 0.5f, S::Rate }, { "BOUNCE", 0.7f, S::Feedback }, { "KICK", 0.8f, S::Amount }, { "DAMP", 0.2f, S::Damping } }},
        { { "TYPE", { "BALL", "PENDULUM", "SPRING" }, 0, MS::Segmented }, { "TRIG", { "NOTE", "FREE" }, 0, MS::Toggle } },
        false, false));
    return a;
}

const std::array<ModuleSpec, kNumModuleTypes>& specs()
{
    static const auto table = buildSpecs();
    return table;
}

// ── formatting helpers ─────────────────────────────────────────────────────────────────────────
const char* kMinus = "\xE2\x88\x92"; // U+2212

std::string fmt (const char* f, double v) { char b[48]; std::snprintf (b, sizeof (b), f, v); return b; }

std::string pct (float v) { return fmt ("%.0f%%", std::round (dsp::clamp01 (v) * 100.0f)); }

std::string signedInt (int x)
{
    if (x == 0) return "0";
    return (x > 0 ? std::string ("+") : std::string (kMinus)) + std::to_string (std::abs (x));
}

std::string signedPct (float bip) { return signedInt ((int) std::lround (bip * 100.0f)) + "%"; }

std::string semitones (float st)
{
    const float r = std::round (st);
    if (std::abs (st - r) < 0.05f)
    {
        const int i = (int) r;
        return (i == 0 ? std::string ("0") : signedInt (i)) + " st";
    }
    return (st > 0 ? std::string ("+") : std::string (kMinus)) + fmt ("%.1f", std::abs (st)) + " st";
}

std::string hertz (float hz, bool unit)
{
    std::string s;
    if (hz >= 10000.0f)      s = fmt ("%.0fk", hz / 1000.0f);
    else if (hz >= 1000.0f)  s = fmt ("%.1fk", hz / 1000.0f);
    else if (hz >= 100.0f)   s = fmt ("%.0f", hz);
    else if (hz >= 10.0f)    s = fmt ("%.1f", hz);
    else                     s = fmt ("%.2f", hz);
    return unit ? s + " Hz" : s;
}

std::string seconds (float sec)
{
    if (sec < 0.0095f)
    {
        const float ms = sec * 1000.0f;
        return std::abs (ms - std::round (ms)) < 0.05f ? fmt ("%.0f ms", ms) : fmt ("%.1f ms", ms);
    }
    if (sec < 0.9995f) return fmt ("%.0f ms", sec * 1000.0f);
    if (sec < 9.95f)   return fmt ("%.1f s", sec);
    return fmt ("%.0f s", sec);
}

std::string decibels (float db, int decimals = 0)
{
    const float r = decimals == 0 ? std::round (db) : std::round (db * 10.0f) / 10.0f;
    if (std::abs (r) < 0.05f) return "0 dB";
    std::string n = decimals == 0 ? fmt ("%.0f", std::abs (r)) : fmt ("%.1f", std::abs (r));
    return (r > 0 ? "+" : kMinus) + n + " dB";
}

std::string lower (std::string s) { for (auto& c : s) c = (char) std::tolower ((unsigned char) c); return s; }

int modeOf (const int* modes, int i) { return modes ? modes[i] : 0; }

std::string syncOrHz (float v, bool sync, float hz)
{
    return sync ? std::string (maps::kSyncRates[maps::syncRateIndex (v)].name) : hertz (hz, true);
}

} // namespace

const ModuleSpec& getSpec (ModuleType t) noexcept
{
    const auto i = (size_t) t;
    return i < specs().size() ? specs()[i] : specs()[0];
}

const std::vector<ModuleType>& cellModuleTypes()
{
    static const std::vector<ModuleType> v {
        ModuleType::Osc, ModuleType::Wave, ModuleType::Noise, ModuleType::Grain, ModuleType::Input, ModuleType::Pluck,
        ModuleType::Filter, ModuleType::Fold, ModuleType::Drive, ModuleType::Ring, ModuleType::Comb, ModuleType::Formant,
        ModuleType::Delay, ModuleType::Verb, ModuleType::Shift, ModuleType::Freeze, ModuleType::Chorus, ModuleType::Loop,
        ModuleType::In };
    return v;
}

const std::vector<ModuleType>& controlModuleTypes()
{
    static const std::vector<ModuleType> v {
        ModuleType::Lfo, ModuleType::Env, ModuleType::Rand, ModuleType::Seq, ModuleType::Follow,
        ModuleType::Drift, ModuleType::Logic, ModuleType::Math, ModuleType::Clock, ModuleType::Physics };
    return v;
}

std::string noteName (int n)
{
    static const char* names[] = { "C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B" };
    n = std::max (0, std::min (127, n));
    return std::string (names[n % 12]) + std::to_string (n / 12 - 2);
}

std::string cellId (int cell)
{
    return std::string (1, laneLetter (laneOf (cell))) + std::to_string (posInLane (cell) + 1);
}

std::string paramLabel (ModuleType t, int k, const int* modes)
{
    const auto& s = getSpec (t);
    if (k < 0 || k >= kNumParams) return {};
    switch (t)
    {
        case ModuleType::Filter:
        {
            const int type = modeOf (modes, 0);
            const bool band = type == 1 || type == 3;
            if (k == 0) return band ? "FREQ" : "CUT";
            if (k == 1) return band ? "Q" : "RES";
            break;
        }
        case ModuleType::Drive:
            if (modeOf (modes, 0) == 3) { if (k == 1) return "RATE"; if (k == 2) return "BITS"; }
            break;
        case ModuleType::Ring:
            if (k == 0 && modeOf (modes, 0) == 1) return "SHIFT";
            break;
        case ModuleType::Comb:
            if (k == 0 && modeOf (modes, 0) == 1) return "FREQ";
            break;
        default: break;
    }
    return s.params[k].label;
}

std::string formatParam (ModuleType t, int k, float v, const int* modes)
{
    v = dsp::clamp01 (v);
    const int m0 = modeOf (modes, 0), m1 = modeOf (modes, 1);
    switch (t)
    {
        case ModuleType::Osc:
            if (k == 0) return semitones (maps::semis24 (v));
            if (k == 2) return fmt ("%.0f ct", maps::oscDetuneCents (v));
            return pct (v);
        case ModuleType::Wave:
            if (k == 0) return fmt ("%.0f", std::round (v * 100.0f));
            if (k == 1) return semitones (maps::semis24 (v));
            if (k == 2) return signedInt ((int) std::lround (maps::bipolar (v) * 100.0f));
            return pct (v);
        case ModuleType::Noise:
            if (k == 0) return v < 0.15f ? "white" : v < 0.45f ? "pink" : v < 0.75f ? "brown" : "dark";
            if (k == 1) { const float g = maps::noiseLevelGain (v); return g <= 1.0e-4f ? std::string (kMinus) + "∞ dB" : decibels (dsp::gainToDb (g)); }
            return pct (v);
        case ModuleType::Grain:
            if (k == 0) return fmt ("%.2f", v);
            if (k == 1) return seconds (maps::grainSizeMs (v) * 0.001f);
            if (k == 2) return fmt ("%.0f/s", maps::grainDensity (v));
            return pct (v);
        case ModuleType::Input:
            if (k == 0) return decibels (maps::inputGainDb (v));
            if (k == 1) return v <= 0.001f ? "off" : decibels (maps::inputGateDb (v));
            if (k == 2) return signedInt ((int) std::lround (maps::bipolar (v) * 100.0f));
            return pct (v);
        case ModuleType::Pluck:
            if (k == 0) return semitones (maps::semis24 (v));
            if (k == 1) return seconds (maps::pluckDecaySec (v));
            return pct (v);
        case ModuleType::In:
            return pct (v);
        case ModuleType::Filter:
        {
            if (k == 0) return hertz (maps::filterCutoffHz (v), false);
            const bool band = m0 == 1 || m0 == 3;
            if (k == 1) return band ? fmt ("%.1f", maps::filterQ (v)) : pct (v);
            return pct (v);
        }
        case ModuleType::Fold:
            if (k == 0) return fmt ("%.1f", maps::foldGain (v)) + "\xC3\x97"; // ×
            if (k == 1) return signedInt ((int) std::lround (maps::bipolar (v) * 100.0f));
            return pct (v);
        case ModuleType::Drive:
            if (k == 0) return decibels (maps::driveGainDb (v));
            if (m0 == 3 && k == 2) return fmt ("%.0f bit", std::round (maps::crushBits (v)));
            return pct (v);
        case ModuleType::Ring:
            if (k == 0) return m1 == 1 ? std::string ("\xC3\x97") + fmt ("%.2f", maps::ringRatio (v)) : hertz (maps::ringFreqHz (v), true);
            if (k == 2) return semitones (maps::bipolar (v));
            return pct (v);
        case ModuleType::Comb:
            if (k == 0)
            {
                if (m0 == 0) return noteName (maps::combNote (v));
                if (m0 == 1) return hertz (maps::combFreeHz (v), true);
                return semitones (maps::semis24 (v));
            }
            return pct (v);
        case ModuleType::Formant:
            if (k == 0) { static const char* vw[] = { "A", "E", "I", "O", "U" }; return vw[(int) std::lround (v * 4.0f)]; }
            if (k == 2) return semitones (maps::semis12 (v));
            return pct (v);
        case ModuleType::Delay:
            if (k == 0) return m1 == 1 ? std::string (dsp::kDivisions[dsp::divisionIndex (v)].name) : seconds (maps::delayFreeMs (v) * 0.001f);
            if (k == 1) return v >= 0.995f ? "∞" : pct (v);
            return pct (v);
        case ModuleType::Verb:
            if (k == 0) return maps::verbInfinite (v) ? "∞" : seconds (maps::verbDecaySec (v));
            if (k == 2) return fmt ("%.0f ms", maps::verbPreMs (v));
            return pct (v);
        case ModuleType::Shift:
            if (k == 0) return semitones (maps::semis24 (v));
            return pct (v);
        case ModuleType::Freeze:
            if (k == 0) return v >= 0.5f ? "ON" : "OFF";
            if (k == 1) return seconds (maps::freezeFadeSec (v));
            if (k == 2) return seconds (maps::freezeSizeMs (v) * 0.001f);
            return pct (v);
        case ModuleType::Chorus:
            if (k == 0) return hertz (maps::chorusRateHz (v), true);
            return pct (v);
        case ModuleType::Loop:
            if (k == 0) { const float s = maps::loopSpeed (v); return (s < -0.005f ? std::string (kMinus) : std::string (s > 0.005f ? "+" : "")) + fmt ("%.2f", std::abs (s)) + "\xC3\x97"; }
            if (k == 1) return m1 == 1 ? std::string (maps::kSyncRates[maps::syncRateIndex (v)].name) : seconds (maps::loopLengthSec (v));
            return pct (v);
        case ModuleType::Lfo:
            if (k == 0) return syncOrHz (v, m1 == 1, maps::lfoRateHz (v));
            if (k == 2) return fmt ("%.0f°", std::round (v * 360.0f));
            if (k == 3) return seconds (v * 10.0f);
            return pct (v);
        case ModuleType::Env:
            if (k == 0) return seconds (maps::envAttackSec (v));
            if (k == 2) return pct (v);
            return seconds (maps::envTimeSec (v));
        case ModuleType::Rand:
            if (k == 0) return syncOrHz (v, m1 == 1, maps::randRateHz (v));
            return pct (v);
        case ModuleType::Seq:
            if (k == 0) return syncOrHz (v, m1 == 1, maps::randRateHz (v));
            if (k == 1) return std::to_string (maps::seqSteps (v));
            return pct (v);
        case ModuleType::Follow:
            if (k == 0) return seconds (maps::followAttackSec (v));
            if (k == 1) return seconds (maps::followReleaseSec (v));
            if (k == 2) return decibels (maps::followGainDb (v));
            return pct (v);
        case ModuleType::Drift:
            if (k == 0) { const float hz = maps::driftRateHz (v, m0); return hz < 10.0f ? fmt ("%.2f", hz) : fmt ("%.1f", hz); }
            if (k == 3) return signedInt ((int) std::lround (maps::bipolar (v) * 100.0f));
            return pct (v);
        case ModuleType::Logic:
            if (k == 3) return v <= 0.001f ? "off" : seconds (maps::envTimeSec (v) * 0.1f);
            return pct (v);
        case ModuleType::Math:
            if (k == 2) { const int q = (int) std::lround (v * 16.0f); return q == 0 ? std::string ("off") : std::to_string (q); }
            return pct (v);
        case ModuleType::Clock:
            if (k == 0) return syncOrHz (v, m1 == 1, maps::randRateHz (v));
            if (k == 1) return std::to_string (maps::clockSteps (v));
            if (k == 2) return std::to_string (maps::clockFill (v, maps::clockSteps (0.5f)));
            return pct (v);
        case ModuleType::Physics:
            return pct (v);
        default: break;
    }
    return pct (v);
}

std::string subLabel (ModuleType t, const float* p, const int* modes, const LabelContext& ctx)
{
    const int m0 = modeOf (modes, 0), m1 = modeOf (modes, 1);
    const auto& s = getSpec (t);
    auto opt = [&s] (int mi, int idx) -> std::string
    {
        if (mi >= s.numModes) return {};
        const auto& o = s.modes[mi].options;
        return idx >= 0 && idx < (int) o.size() ? std::string (o[(size_t) idx]) : std::string();
    };
    switch (t)
    {
        case ModuleType::Osc:     return lower (opt (0, m0)) + (m1 == 1 ? " · sync" : "");
        case ModuleType::Wave:    return ctx.resourceName.empty() ? opt (0, m0) : ctx.resourceName;
        case ModuleType::Noise:   return formatParam (t, 0, p ? p[0] : 0.3f, modes);
        case ModuleType::Grain:   return ctx.resourceName.empty() ? "no sample" : ctx.resourceName;
        case ModuleType::Input:   return "side-chain";
        case ModuleType::Pluck:   return m0 == 0 ? "string" : (m0 == 1 ? "bowed" : "resonator");
        case ModuleType::In:
        {
            std::string r = "\xE2\x86\x90 "; // ←
            bool first = true;
            for (int l = 0; l < kNumLanes; ++l)
                if (ctx.inboundMask & (1u << l)) { r += (first ? "" : " + "); r += laneLetter (l); first = false; }
            return first ? std::string ("no input") : r;
        }
        case ModuleType::Filter:  return opt (0, m0) + (m1 == 1 ? " 24" : " 12");
        case ModuleType::Fold:    return opt (0, m0) + (m0 == 0 ? " stage" : " stages");
        case ModuleType::Drive:   return lower (opt (0, m0));
        case ModuleType::Ring:    return lower (opt (0, m0)) + (m1 == 1 ? " · key" : "");
        case ModuleType::Comb:    return lower (opt (0, m0));
        case ModuleType::Formant: return "vowel " + formatParam (t, 0, p ? p[0] : 0.0f, modes);
        case ModuleType::Delay:   return (p ? formatParam (t, 0, p[0], modes) : std::string()) + " " + lower (opt (0, m0));
        case ModuleType::Verb:    return lower (opt (0, m0));
        case ModuleType::Shift:   return p ? formatParam (t, 0, p[0], modes) : std::string();
        case ModuleType::Freeze:  return (p && p[0] >= 0.5f) ? "frozen" : "armed";
        case ModuleType::Chorus:  return lower (opt (0, m0));
        case ModuleType::Loop:    return lower (opt (0, m0));
        case ModuleType::Lfo:     return opt (0, m0) + " · " + (p ? formatParam (t, 0, p[0], modes) : std::string());
        case ModuleType::Env:     return "A " + formatParam (t, 0, p ? p[0] : 0.0f, modes) + " · D " + formatParam (t, 1, p ? p[1] : 0.0f, modes);
        case ModuleType::Rand:    return opt (0, m0) + " · " + (p ? formatParam (t, 0, p[0], modes) : std::string());
        case ModuleType::Seq:     return std::to_string (maps::seqSteps (p ? p[1] : 1.0f)) + " steps · " + (p ? formatParam (t, 0, p[0], modes) : std::string());
        case ModuleType::Follow:  return m0 < 4 ? std::string ("\xE2\x86\x90 lane ") + laneLetter (m0) + " level" : std::string ("\xE2\x86\x90 input level");
        case ModuleType::Drift:   return lower (opt (0, m0)) + " · " + (p ? formatParam (t, 0, p[0], modes) : std::string());
        case ModuleType::Logic:   return "A " + opt (0, m0) + " B";
        case ModuleType::Math:    return lower (opt (0, m0)) + " A, B";
        case ModuleType::Clock:   return m0 == 1 ? "euclid " + std::to_string (maps::clockFill (p ? p[2] : 0.5f, maps::clockSteps (p ? p[1] : 0.5f))) + "/" + std::to_string (maps::clockSteps (p ? p[1] : 0.5f))
                                                 : (p ? formatParam (t, 0, p[0], modes) : std::string());
        case ModuleType::Physics: return lower (opt (0, m0));
        default: break;
    }
    return {};
}

void mainParams (ModuleType t, const LabelContext& ctx, int& first, int& second)
{
    first = 0; second = 1;
    if (t != ModuleType::In) return;
    int found[2] = { -1, -1 }, n = 0;
    for (int l = 0; l < kNumLanes && n < 2; ++l)
        if (ctx.inboundMask & (1u << l)) found[n++] = l;
    if (n >= 1) first = found[0];
    if (n >= 2) second = found[1];
    else second = first == 0 ? 1 : 0;
    if (first > second && n < 2) std::swap (first, second);
}

} // namespace weft
