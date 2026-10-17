#include "Patch.h"
#include "DspUtil.h"

#include <algorithm>
#include <cmath>

namespace weft
{
Patch::Patch()
{
    static const char* laneNames[] = { "VOICE", "TEXTURE", "SPACE", "BED" };
    for (int l = 0; l < kNumLanes; ++l)
        lanes[(size_t) l].name = laneNames[l];
}

static void defaultAux (ModuleType t, std::array<float, kAuxSize>& aux)
{
    aux.fill (0.0f);
    if (t == ModuleType::Seq)
    {
        const float steps[8] = { 0.9f, 0.3f, 0.6f, 0.2f, 0.8f, 0.45f, 0.7f, 0.15f };
        for (int i = 0; i < 8; ++i) { aux[(size_t) i] = steps[i]; aux[(size_t) (8 + i)] = 1.0f; } // values, probabilities
    }
    else if (t == ModuleType::Logic || t == ModuleType::Math)
    {
        aux[0] = 0.0f;   // input A: slot 1
        aux[1] = 1.0f;   // input B: slot 2
    }
}

void assignModule (ModuleData& m, ModuleType newType, bool migrate)
{
    const ModuleType oldType = m.type;
    const auto oldParams = m.params;
    const auto& ns = getSpec (newType);
    const auto& os = getSpec (oldType);

    std::array<bool, kNumParams> used {};
    for (int k = 0; k < kNumParams; ++k)
    {
        float v = ns.params[k].defaultValue;
        if (migrate && oldType != ModuleType::None && newType != ModuleType::None && ns.params[k].semantic != Semantic::None)
        {
            for (int j = 0; j < kNumParams; ++j)
                if (! used[(size_t) j] && os.params[j].semantic == ns.params[k].semantic)
                {
                    v = oldParams[(size_t) j];
                    used[(size_t) j] = true;
                    break;
                }
        }
        m.params[(size_t) k] = dsp::clamp01 (v);
    }
    for (int i = 0; i < kNumModes; ++i)
        m.modes[(size_t) i] = i < ns.numModes ? ns.modes[i].defaultIndex : 0;
    defaultAux (newType, m.aux);
    if (newType != oldType && ! (newType == ModuleType::Grain || newType == ModuleType::Wave))
        m.resource.reset();
    m.type = newType;
}

bool sourceExists (const Patch& p, SourceRef s)
{
    switch (s.kind)
    {
        case SourceKind::Slot:  return s.index < kNumSlots && ! p.slots[s.index].empty();
        case SourceKind::Macro: return s.index < kNumMacros;
        case SourceKind::Audio: return s.index < kNumCells && ! p.cells[s.index].empty();
        case SourceKind::Midi:  return s.index < kNumMidiSources;
    }
    return false;
}

bool destExists (const Patch& p, DestRef d)
{
    if (d.param >= kNumParams) return false;
    if (d.kind == DestKind::Cell) return d.index < kNumCells && ! p.cells[d.index].empty();
    return d.index < kNumSlots && ! p.slots[d.index].empty();
}

void sanitizeConnections (Patch& p)
{
    std::vector<Connection> out;
    out.reserve (p.connections.size());
    for (const auto& c : p.connections)
    {
        if (! sourceExists (p, c.src) || ! destExists (p, c.dst)) continue;
        if (! std::isfinite (c.depth)) continue;
        // a modulator modulating its own parameters is allowed (self-modulation); duplicates are not
        bool dup = false;
        for (const auto& o : out) if (o.src == c.src && o.dst == c.dst) { dup = true; break; }
        if (dup) continue;
        if ((int) out.size() >= kMaxConnections) break;
        auto cc = c;
        cc.depth = dsp::clampf (c.depth, -1.0f, 1.0f);
        out.push_back (cc);
    }
    p.connections = std::move (out);
}

int findConnection (const Patch& p, SourceRef s, DestRef d)
{
    for (size_t i = 0; i < p.connections.size(); ++i)
        if (p.connections[i].src == s && p.connections[i].dst == d) return (int) i;
    return -1;
}

uint8_t inboundMask (const Patch& p, int lane)
{
    uint8_t m = 0;
    for (int s = 0; s < kNumLanes; ++s)
        if (p.lanes[(size_t) s].toLane[(size_t) lane]) m |= (uint8_t) (1u << s);
    return m;
}

void captureMorphable (const Patch& p, std::array<float, kNumMorphable>& v)
{
    for (int c = 0; c < kNumCells; ++c)
        for (int k = 0; k < kNumParams; ++k) v[(size_t) (c * kNumParams + k)] = p.cells[(size_t) c].params[(size_t) k];
    for (int s = 0; s < kNumSlots; ++s)
        for (int k = 0; k < kNumParams; ++k) v[(size_t) (kNumCells * kNumParams + s * kNumParams + k)] = p.slots[(size_t) s].params[(size_t) k];
}

void applyMorphable (Patch& p, const std::array<float, kNumMorphable>& v)
{
    for (int c = 0; c < kNumCells; ++c)
        for (int k = 0; k < kNumParams; ++k) p.cells[(size_t) c].params[(size_t) k] = dsp::clamp01 (v[(size_t) (c * kNumParams + k)]);
    for (int s = 0; s < kNumSlots; ++s)
        for (int k = 0; k < kNumParams; ++k) p.slots[(size_t) s].params[(size_t) k] = dsp::clamp01 (v[(size_t) (kNumCells * kNumParams + s * kNumParams + k)]);
}

std::array<float, kNumScenes> morphWeights (float x, float y) noexcept
{
    x = dsp::clamp01 (x); y = dsp::clamp01 (y);
    return { (1.0f - x) * (1.0f - y), x * (1.0f - y), (1.0f - x) * y, x * y };
}

void sceneCorner (int scene, float& x, float& y) noexcept
{
    x = (scene == 1 || scene == 3) ? 1.0f : 0.0f;
    y = (scene == 2 || scene == 3) ? 1.0f : 0.0f;
}

Patch makeInitPatch()
{
    Patch p;
    p.name = "Init";
    p.category = "INIT";
    assignModule (p.cells[0], ModuleType::Wave, false);
    assignModule (p.cells[1], ModuleType::Filter, false);
    assignModule (p.slots[0], ModuleType::Lfo, false);
    assignModule (p.slots[1], ModuleType::Env, false);
    p.macros[0].name = "BLOOM";
    p.macros[1].name = "GRIT";
    p.macros[2].name = "SPACE";
    p.macros[3].name = "TIME";
    for (int i = 0; i < kNumMacros; ++i) p.macros[(size_t) i].cc = 20 + i;
    std::array<float, kNumMorphable> v {};
    captureMorphable (p, v);
    for (auto& s : p.scenes) { s.stored = true; s.values = v; }
    p.selectedScene = 0;
    return p;
}

} // namespace weft
