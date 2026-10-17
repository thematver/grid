// The design's demo patch values must read exactly as they do in the design.
#include "Test.h"
#include "engine/ModuleSpec.h"
#include "engine/ParamMaps.h"
#include "engine/Patch.h"

using namespace weft;

static std::string f (ModuleType t, int k, float v, int m0 = -1, int m1 = -1)
{
    const auto& s = getSpec (t);
    int modes[2] = { m0 >= 0 ? m0 : (s.numModes > 0 ? s.modes[0].defaultIndex : 0), m1 >= 0 ? m1 : (s.numModes > 1 ? s.modes[1].defaultIndex : 0) };
    return formatParam (t, k, v, modes);
}

TEST ("spec: design labels for cell parameters")
{
    CHECK_EQ_STR (f (ModuleType::Wave, 0, 0.42f), "42");
    CHECK_EQ_STR (f (ModuleType::Wave, 1, 0.5f), "0 st");
    CHECK_EQ_STR (f (ModuleType::Fold, 0, 0.35f), "1.8\xC3\x97");
    CHECK_EQ_STR (f (ModuleType::Fold, 1, 0.5f), "0");
    CHECK_EQ_STR (f (ModuleType::Filter, 1, 0.38f), "38%");
    CHECK_EQ_STR (f (ModuleType::Grain, 0, 0.61f), "0.61");
    CHECK_EQ_STR (f (ModuleType::Grain, 1, 0.3f), "80 ms");
    CHECK_EQ_STR (f (ModuleType::Comb, 0, 0.5f), "C3");
    CHECK_EQ_STR (f (ModuleType::Comb, 1, 0.7f), "70%");
    CHECK_EQ_STR (f (ModuleType::Drive, 0, 0.4f), "+6 dB");
    CHECK_EQ_STR (f (ModuleType::Drive, 1, 0.6f), "60%");
    CHECK_EQ_STR (f (ModuleType::In, 0, 0.8f), "80%");
    CHECK_EQ_STR (f (ModuleType::Delay, 0, 0.45f), "3/16");
    CHECK_EQ_STR (f (ModuleType::Delay, 1, 0.62f), "62%");
    CHECK_EQ_STR (f (ModuleType::Verb, 0, 0.8f), "9.0 s");
    CHECK_EQ_STR (f (ModuleType::Verb, 1, 0.4f), "40%");
    CHECK_EQ_STR (f (ModuleType::Freeze, 0, 0.0f), "OFF");
    CHECK_EQ_STR (f (ModuleType::Freeze, 1, 0.5f), "1.2 s");
    CHECK_EQ_STR (f (ModuleType::Noise, 0, 0.3f), "pink");
    CHECK_EQ_STR (f (ModuleType::Noise, 1, 0.4f), "\xE2\x88\x92" "14 dB");
}

TEST ("spec: design sub labels")
{
    LabelContext none;
    auto sub = [&] (ModuleType t, std::array<float, 4> p, int m0, int m1, const LabelContext& c)
    {
        int modes[2] = { m0, m1 };
        return subLabel (t, p.data(), modes, c);
    };
    CHECK_EQ_STR (sub (ModuleType::Filter, { 0.55f, 0.38f, 0, 0 }, 0, 1, none), "LP 24");
    CHECK_EQ_STR (sub (ModuleType::Filter, { 0.62f, 0.7f, 0, 0 }, 1, 0, none), "BP 12");
    CHECK_EQ_STR (sub (ModuleType::Fold, { 0.35f, 0.5f, 0, 0 }, 1, 0, none), "2 stages");
    CHECK_EQ_STR (sub (ModuleType::Comb, { 0.5f, 0.7f, 0, 0 }, 0, 0, none), "tuned");
    CHECK_EQ_STR (sub (ModuleType::Drive, { 0.4f, 0.6f, 0, 0 }, 0, 0, none), "tape");
    CHECK_EQ_STR (sub (ModuleType::Delay, { 0.45f, 0.62f, 0, 0 }, 0, 1, none), "3/16 ping");
    CHECK_EQ_STR (sub (ModuleType::Verb, { 0.8f, 0.4f, 0, 0 }, 1, 0, none), "hall");
    CHECK_EQ_STR (sub (ModuleType::Freeze, { 0.0f, 0.5f, 0, 0 }, 0, 0, none), "armed");
    CHECK_EQ_STR (sub (ModuleType::Noise, { 0.3f, 0.4f, 0, 0 }, 0, 0, none), "pink");
    LabelContext ab; ab.inboundMask = 0b0011;
    CHECK_EQ_STR (sub (ModuleType::In, { 0.8f, 0.6f, 1, 1 }, 0, 0, ab), "\xE2\x86\x90 A + B");
    LabelContext grain; grain.resourceName = "rain_tin.wav";
    CHECK_EQ_STR (sub (ModuleType::Grain, { 0.61f, 0.3f, 0, 0 }, 0, 1, grain), "rain_tin.wav");
    LabelContext bell; bell.resourceName = "Glass Bell";
    CHECK_EQ_STR (sub (ModuleType::Wave, { 0.42f, 0.5f, 0, 0 }, 0, 0, bell), "Glass Bell");
}

TEST ("spec: design modulator sub labels")
{
    LabelContext none;
    auto sub = [&] (ModuleType t, std::array<float, 4> p, int m0, int m1)
    {
        int modes[2] = { m0, m1 };
        return subLabel (t, p.data(), modes, none);
    };
    const float lfoRate = std::log (30.0f) / std::log (5000.0f);   // 0.30 Hz
    CHECK_EQ_STR (sub (ModuleType::Lfo, { lfoRate, 0.5f, 0, 0 }, 0, 0), "TRI \xC2\xB7 0.30 Hz");
    CHECK_EQ_STR (sub (ModuleType::Env, { 0.21f, 0.716f, 0, 0.6f }, 0, 0), "A 4 ms \xC2\xB7 D 1.2 s");
    CHECK_EQ_STR (sub (ModuleType::Rand, { 6.0f / 19.0f, 0, 1, 1 }, 0, 1), "S&H \xC2\xB7 1/8");
    CHECK_EQ_STR (sub (ModuleType::Seq, { 3.0f / 19.0f, 1.0f, 0, 1 }, 0, 1), "8 steps \xC2\xB7 1/16");
    CHECK_EQ_STR (sub (ModuleType::Follow, { 0.3f, 0.5f, 0.3f, 0 }, 1, 1), "\xE2\x86\x90 lane B level");
    CHECK_EQ_STR (sub (ModuleType::Drift, { 0.5f, 1, 0.5f, 0.5f }, 0, 0), "slow \xC2\xB7 0.02");
}

TEST ("spec: filter Q mapping reads Q 1.8 at 38% resonance")
{
    CHECK_NEAR (maps::filterQ (0.38f), 1.84, 0.01);
    CHECK_NEAR (maps::filterCutoffHz (0.0f), 20.0, 0.01);
    CHECK_NEAR (maps::filterCutoffHz (1.0f), 20000.0, 1.0);
}

TEST ("spec: every module has a name, four labels and sane defaults")
{
    for (int i = 1; i < kNumModuleTypes; ++i)
    {
        const auto& s = getSpec ((ModuleType) i);
        CHECK_MSG (s.name && *s.name, std::to_string (i));
        for (int k = 0; k < kNumParams; ++k)
        {
            CHECK_MSG (s.params[k].label && *s.params[k].label, s.name);
            CHECK (s.params[k].defaultValue >= 0.0f && s.params[k].defaultValue <= 1.0f);
            int modes[2] = { 0, 0 };
            CHECK (! formatParam ((ModuleType) i, k, 0.0f, modes).empty());
            CHECK (! formatParam ((ModuleType) i, k, 1.0f, modes).empty());
        }
        for (int m = 0; m < s.numModes; ++m)
            CHECK (s.modes[m].defaultIndex >= 0 && s.modes[m].defaultIndex < (int) s.modes[m].options.size());
    }
}

TEST ("patch: migration keeps values by meaning")
{
    ModuleData m;
    assignModule (m, ModuleType::Filter, false);
    m.params[0] = 0.77f;     // CUT (Frequency)
    m.params[1] = 0.66f;     // RES
    assignModule (m, ModuleType::Ring, true);   // FREQ is a Frequency too
    CHECK_NEAR (m.params[0], 0.77, 1e-6);
    assignModule (m, ModuleType::Delay, true);  // no frequency -> defaults
    CHECK_NEAR (m.params[0], getSpec (ModuleType::Delay).params[0].defaultValue, 1e-6);
}

TEST ("patch: morph weights are bilinear and exact at corners")
{
    for (int s = 0; s < kNumScenes; ++s)
    {
        float x, y; sceneCorner (s, x, y);
        const auto w = morphWeights (x, y);
        for (int t = 0; t < kNumScenes; ++t) CHECK_NEAR (w[(size_t) t], s == t ? 1.0 : 0.0, 1e-6);
    }
    const auto w = morphWeights (0.3f, 0.6f);
    CHECK_NEAR (w[0] + w[1] + w[2] + w[3], 1.0, 1e-6);
}
