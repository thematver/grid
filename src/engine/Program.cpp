#include "Program.h"

namespace weft
{
void CellInstances::prepare (double sr)
{
    if (sr <= 0.0) return;
    if (global) { global->prepare (sr); global->reset(); }
    for (auto& v : voice) if (v) { v->prepare (sr); v->reset(); }
    preparedRate = sr;
}

void SlotInstances::prepare (double sr)
{
    if (sr <= 0.0) return;
    if (global) { global->prepare (sr); global->reset(); }
    for (auto& v : voice) if (v) { v->prepare (sr); v->reset(); }
    preparedRate = sr;
}

static std::shared_ptr<CellInstances> makeCell (ModuleType t, double sr)
{
    auto ci = std::make_shared<CellInstances>();
    ci->type = t;
    ci->global = createAudioModule (t);
    if (getSpec (t).polyCapable)
        for (auto& v : ci->voice) v = createAudioModule (t);
    ci->prepare (sr);
    return ci;
}

static bool controlHasVoices (ModuleType t) { return t == ModuleType::Env || t == ModuleType::Lfo; }

static std::shared_ptr<SlotInstances> makeSlot (ModuleType t, double sr)
{
    auto si = std::make_shared<SlotInstances>();
    si->type = t;
    si->global = createControlModule (t);
    if (controlHasVoices (t))
        for (auto& v : si->voice) v = createControlModule (t);
    si->prepare (sr);
    return si;
}

void InstanceRegistry::setSampleRate (double sr)
{
    std::lock_guard<std::mutex> g (lock);
    rate = sr;
    for (auto& c : cells) if (c) c->prepare (sr);
    for (auto& s : slots) if (s) s->prepare (sr);
}

void InstanceRegistry::clear()
{
    std::lock_guard<std::mutex> g (lock);
    for (auto& c : cells) c.reset();
    for (auto& s : slots) s.reset();
}

bool laneIsVoiced (const Patch& p, int lane)
{
    if (p.lanes[(size_t) lane].trigger != LaneTrigger::Notes) return false;
    for (int i = 0; i < kCellsPerLane; ++i)
    {
        const auto t = p.cells[(size_t) cellIndex (lane, i)].type;
        if (t == ModuleType::None) continue;
        const auto& s = getSpec (t);
        if (s.family == Family::Space) return false;   // sources after the first SPACE module do not count
        if (s.noteSource) return true;
    }
    return false;
}

int laneVoicedCount (const Patch& p, int lane)
{
    if (! laneIsVoiced (p, lane)) return 0;
    int n = 0;
    for (int i = 0; i < kCellsPerLane; ++i)
    {
        const auto t = p.cells[(size_t) cellIndex (lane, i)].type;
        if (t == ModuleType::None) continue;
        if (! getSpec (t).polyCapable) break;
        ++n;
    }
    return n;
}

static void addRoute (ParamRouting& r, const Connection& c)
{
    const int total = r.count + r.audioCount;
    if (total >= kMaxModsPerParam || c.id < 0 || c.id >= kMaxConnections) return;
    ModInput in { c.src.kind, c.src.index, (uint8_t) c.id };
    if (c.src.kind == SourceKind::Audio)
    {
        r.in[total] = in;
        ++r.audioCount;
    }
    else
    {
        // keep control inputs contiguous at the front
        for (int i = total; i > r.count; --i) r.in[i] = r.in[i - 1];
        r.in[r.count] = in;
        ++r.count;
    }
}

std::unique_ptr<Program> InstanceRegistry::compile (const Patch& patch)
{
    std::lock_guard<std::mutex> g (lock);
    auto prog = std::make_unique<Program>();
    prog->serial = ++serial;

    for (int c = 0; c < kNumCells; ++c)
    {
        const auto t = patch.cells[(size_t) c].type;
        if (t == ModuleType::None || ! isAudioType (t)) { cells[(size_t) c].reset(); continue; }
        if (! cells[(size_t) c] || cells[(size_t) c]->type != t)
            cells[(size_t) c] = makeCell (t, rate);
        auto& cp = prog->cells[c];
        cp.type = t;
        cp.inst = cells[(size_t) c].get();
        prog->keepCells.push_back (cells[(size_t) c]);
        if (patch.cells[(size_t) c].resource)
        {
            cp.resource = patch.cells[(size_t) c].resource.get();
            prog->keepResources.push_back (patch.cells[(size_t) c].resource);
        }
    }

    for (int s = 0; s < kNumSlots; ++s)
    {
        const auto t = patch.slots[(size_t) s].type;
        if (t == ModuleType::None || ! isControlType (t)) { slots[(size_t) s].reset(); continue; }
        if (! slots[(size_t) s] || slots[(size_t) s]->type != t)
            slots[(size_t) s] = makeSlot (t, rate);
        prog->slots[s].type = t;
        prog->slots[s].inst = slots[(size_t) s].get();
        prog->keepSlots.push_back (slots[(size_t) s]);
    }

    for (int l = 0; l < kNumLanes; ++l)
    {
        auto& lp = prog->lanes[l];
        const auto& ld = patch.lanes[(size_t) l];
        lp.toMix = ld.toMix;
        lp.toLaneMask = 0;
        for (int d = 0; d < kNumLanes; ++d) if (ld.toLane[(size_t) d]) lp.toLaneMask |= (uint8_t) (1u << d);
        lp.inboundMask = inboundMask (patch, l);

        for (int i = 0; i < kCellsPerLane; ++i)
        {
            const int c = cellIndex (l, i);
            if (prog->cells[c].type == ModuleType::None) continue;
            if (prog->cells[c].type == ModuleType::In && lp.entry < 0) lp.entry = lp.numCells;
            prog->cells[c].inboundMask = lp.inboundMask;
            lp.cells[lp.numCells++] = c;
        }
        lp.voiced = laneIsVoiced (patch, l);
        lp.voicedCount = lp.voiced ? laneVoicedCount (patch, l) : 0;
        if (lp.voicedCount == 0) lp.voiced = false;
        lp.outputPerVoice = lp.voiced && lp.voicedCount == lp.numCells;
        for (int i = 0; i < lp.numCells; ++i)
            prog->cells[lp.cells[i]].voiced = i < lp.voicedCount;
        prog->anyVoicedLane |= lp.voiced;
    }

    for (const auto& c : patch.connections)
    {
        if (! sourceExists (patch, c.src) || ! destExists (patch, c.dst)) continue;
        if (c.dst.kind == DestKind::Cell)
        {
            if (prog->cells[c.dst.index].type == ModuleType::None) continue;
            addRoute (prog->cells[c.dst.index].routes[c.dst.param], c);
        }
        else
        {
            if (prog->slots[c.dst.index].type == ModuleType::None) continue;
            addRoute (prog->slots[c.dst.index].routes[c.dst.param], c);
        }
        if (c.src.kind == SourceKind::Audio && c.src.index < kNumCells)
            prog->cells[c.src.index].audioSource = true;
    }
    return prog;
}

} // namespace weft
