// Whole-engine stress: random patches, random routing (incl. lane feedback and audio-rate
// modulation), dense MIDI, odd block sizes, topology changes while playing.
#include "Test.h"
#include "Fuzz.h"
#include "engine/Engine.h"
#include "engine/Patch.h"
#include "engine/Program.h"

#include <chrono>

using namespace weft;
using namespace wtest;

namespace
{
void pushPatch (Engine& e, InstanceRegistry& reg, Patch& p)
{
    sanitizeConnections (p);
    int id = 0;
    for (auto& c : p.connections) c.id = id++;
    auto& s = e.params();
    for (int c = 0; c < kNumCells; ++c)
    {
        for (int k = 0; k < kNumParams; ++k) s.setCellParam (c, k, p.cells[(size_t) c].params[(size_t) k]);
        for (int m = 0; m < kNumModes; ++m) s.cellMode[(size_t) (c * kNumModes + m)].store (p.cells[(size_t) c].modes[(size_t) m]);
        for (int i = 0; i < kAuxSize; ++i) s.cellAux[(size_t) (c * kAuxSize + i)].store (p.cells[(size_t) c].aux[(size_t) i]);
    }
    for (int sl = 0; sl < kNumSlots; ++sl)
    {
        for (int k = 0; k < kNumParams; ++k) s.setSlotParam (sl, k, p.slots[(size_t) sl].params[(size_t) k]);
        for (int m = 0; m < kNumModes; ++m) s.slotMode[(size_t) (sl * kNumModes + m)].store (p.slots[(size_t) sl].modes[(size_t) m]);
        for (int i = 0; i < kAuxSize; ++i) s.slotAux[(size_t) (sl * kAuxSize + i)].store (p.slots[(size_t) sl].aux[(size_t) i]);
    }
    for (const auto& c : p.connections) s.depth[(size_t) c.id].store (c.depth);
    for (int l = 0; l < kNumLanes; ++l)
    {
        s.laneLevel[(size_t) l].store (p.lanes[(size_t) l].level);
        s.laneMute[(size_t) l].store (p.lanes[(size_t) l].mute);
        s.laneFreeze[(size_t) l].store (p.lanes[(size_t) l].freeze);
    }
    s.polyphony.store (p.voice.polyphony);
    s.glide.store (p.voice.glide);
    e.publish (reg.compile (p));
}

Patch randomPatch (dsp::Rng& r)
{
    Patch p;
    const auto& cellTypes = cellModuleTypes();
    const auto& ctlTypes = controlModuleTypes();
    for (int c = 0; c < kNumCells; ++c)
    {
        if (r.uniform() < 0.3f) continue;
        const auto t = cellTypes[r.next() % cellTypes.size()];
        assignModule (p.cells[(size_t) c], t, false);
        for (auto& v : p.cells[(size_t) c].params) if (r.uniform() < 0.5f) v = r.uniform();
        const auto& sp = getSpec (t);
        for (int m = 0; m < sp.numModes; ++m) p.cells[(size_t) c].modes[(size_t) m] = (int) (r.next() % sp.modes[m].options.size());
        for (auto& a : p.cells[(size_t) c].aux) a = r.uniform();
        if (t == ModuleType::Grain) p.cells[(size_t) c].resource = builtinSample (builtinSampleNames()[r.next() % 6]);
    }
    for (int s = 0; s < kNumSlots; ++s)
    {
        if (r.uniform() < 0.3f) continue;
        const auto t = ctlTypes[r.next() % ctlTypes.size()];
        assignModule (p.slots[(size_t) s], t, false);
        for (auto& v : p.slots[(size_t) s].params) v = r.uniform();
        const auto& sp = getSpec (t);
        for (int m = 0; m < sp.numModes; ++m) p.slots[(size_t) s].modes[(size_t) m] = (int) (r.next() % sp.modes[m].options.size());
        for (int i = 0; i < kAuxSize; ++i) p.slots[(size_t) s].aux[(size_t) i] = r.uniform() * 15.0f;
    }
    for (int l = 0; l < kNumLanes; ++l)
    {
        auto& ld = p.lanes[(size_t) l];
        ld.toMix = r.uniform() < 0.8f;
        for (int d = 0; d < kNumLanes; ++d) ld.toLane[(size_t) d] = d != l && r.uniform() < 0.25f;
        ld.level = 0.5f + 0.5f * r.uniform();
        ld.trigger = r.uniform() < 0.85f ? LaneTrigger::Notes : LaneTrigger::Free;
        ld.freeze = r.uniform() < 0.1f;
    }
    const int nConn = (int) (r.next() % 60);
    for (int i = 0; i < nConn; ++i)
    {
        Connection c;
        const float k = r.uniform();
        c.src.kind = k < 0.5f ? SourceKind::Slot : k < 0.7f ? SourceKind::Macro : k < 0.85f ? SourceKind::Audio : SourceKind::Midi;
        c.src.index = (uint8_t) (r.next() % (c.src.kind == SourceKind::Audio ? kNumCells : c.src.kind == SourceKind::Midi ? kNumMidiSources : 8));
        c.dst.kind = r.uniform() < 0.8f ? DestKind::Cell : DestKind::Slot;
        c.dst.index = (uint8_t) (r.next() % (c.dst.kind == DestKind::Cell ? kNumCells : kNumSlots));
        c.dst.param = (uint8_t) (r.next() % kNumParams);
        c.depth = r.bipolar();
        if (findConnection (p, c.src, c.dst) < 0) p.connections.push_back (c);
    }
    p.voice.polyphony = r.uniform() < 0.2f ? 1 : 1 + (int) (r.next() % 8);
    p.voice.glide = r.uniform() < 0.3f ? r.uniform() * 0.3f : 0.0f;
    return p;
}

struct Result { bool finite = true; double peak = 0.0, rms = 0.0, rtRatio = 0.0; };

Result play (Engine& e, InstanceRegistry& reg, Patch& p, dsp::Rng& r, double sr, int seconds, bool randomBlocks, bool mutateTopology)
{
    Result res;
    std::vector<float> L (4096), R (4096), sideL (4096), sideR (4096);
    float* out[2] = { L.data(), R.data() };
    const float* side[2] = { sideL.data(), sideR.data() };
    std::vector<MidiEvent> ev;
    HostTransport ht; ht.valid = true; ht.bpm = 100.0 + 60.0 * r.uniform(); ht.playing = r.uniform() < 0.5f;
    const int64_t total = (int64_t) (sr * seconds);
    int64_t done = 0;
    double sq = 0.0; int64_t cnt = 0;
    double busy = 0.0;
    while (done < total)
    {
        const int n = randomBlocks ? 1 + (int) (r.next() % 1024) : 512;
        ev.clear();
        const int nev = (int) (r.next() % 4);
        for (int k = 0; k < nev; ++k)
        {
            MidiEvent m; m.offset = (int) (r.next() % (uint32_t) n);
            const float t = r.uniform();
            if (t < 0.45f) { m.status = 0x90; m.data1 = (uint8_t) (24 + r.next() % 80); m.data2 = (uint8_t) (1 + r.next() % 127); }
            else if (t < 0.85f) { m.status = 0x80; m.data1 = (uint8_t) (24 + r.next() % 80); }
            else if (t < 0.9f) { m.status = 0xE0; m.data1 = (uint8_t) (r.next() % 128); m.data2 = (uint8_t) (r.next() % 128); }
            else if (t < 0.95f) { m.status = 0xB0; m.data1 = (uint8_t) (r.uniform() < 0.5f ? 64 : 20 + r.next() % 8); m.data2 = (uint8_t) (r.next() % 128); }
            else { m.status = 0xD0; m.data1 = (uint8_t) (r.next() % 128); }
            ev.push_back (m);
        }
        std::sort (ev.begin(), ev.end(), [] (const MidiEvent& a, const MidiEvent& b) { return a.offset < b.offset; });
        for (int i = 0; i < n; ++i) { sideL[(size_t) i] = 0.3f * r.bipolar(); sideR[(size_t) i] = 0.3f * r.bipolar(); }
        const auto t0 = std::chrono::steady_clock::now();
        e.process (out, 2, side, 2, n, ev.data(), (int) ev.size(), ht);
        busy += std::chrono::duration<double> (std::chrono::steady_clock::now() - t0).count();
        ht.ppq += n / sr * ht.bpm / 60.0;
        for (int i = 0; i < n; ++i)
        {
            const float a = L[(size_t) i], b = R[(size_t) i];
            if (! std::isfinite (a) || ! std::isfinite (b)) res.finite = false;
            res.peak = std::max (res.peak, (double) std::max (std::abs (a), std::abs (b)));
            sq += a * a + b * b; cnt += 2;
        }
        done += n;
        if (mutateTopology && r.uniform() < 0.02f)
        {
            Patch q = randomPatch (r);
            pushPatch (e, reg, q);
        }
        if (r.uniform() < 0.1f) e.collectGarbage();
        if (r.uniform() < 0.01f) e.params().morphX.store (r.uniform());
    }
    res.rms = std::sqrt (sq / std::max<int64_t> (1, cnt));
    res.rtRatio = busy / ((double) total / sr);
    return res;
}
} // namespace

TEST ("engine: the init patch sounds")
{
    Engine e; InstanceRegistry reg;
    reg.setSampleRate (48000.0);
    e.prepare (48000.0, 512);
    Patch p = makeInitPatch();
    pushPatch (e, reg, p);
    std::vector<float> L (512), R (512);
    float* out[2] = { L.data(), R.data() };
    MidiEvent on { 0, 0x90, 60, 100 };
    HostTransport ht;
    double sq = 0.0, pk = 0.0;
    for (int b = 0; b < 200; ++b)
    {
        e.process (out, 2, nullptr, 0, 512, b == 0 ? &on : nullptr, b == 0 ? 1 : 0, ht);
        for (int i = 0; i < 512; ++i) { sq += L[(size_t) i] * L[(size_t) i]; pk = std::max (pk, (double) std::abs (L[(size_t) i])); }
    }
    std::printf ("    init patch rms %.3f peak %.3f\n", std::sqrt (sq / (200 * 512)), pk);
    CHECK (std::sqrt (sq / (200 * 512)) > 0.01);
    CHECK (pk <= 1.0);
}

TEST ("engine: random patches stay finite, bounded and real-time")
{
    dsp::Rng r (2024);
    double worstRt = 0.0;
    int silent = 0;
    for (int trial = 0; trial < 60; ++trial)
    {
        const double sr = trial % 4 == 3 ? 96000.0 : (trial % 4 == 2 ? 44100.0 : 48000.0);
        Engine e; InstanceRegistry reg;
        reg.setSampleRate (sr);
        e.prepare (sr, 1024);
        Patch p = randomPatch (r);
        pushPatch (e, reg, p);
        const auto res = play (e, reg, p, r, sr, 3, trial % 2 == 0, trial % 5 == 0);
        CHECK (res.finite);
        CHECK (res.peak <= 1.0);
        if (res.rms < 1e-5) ++silent;
        worstRt = std::max (worstRt, res.rtRatio * (sr / 48000.0 > 1.5 ? 0.5 : 1.0));
    }
    std::printf ("    worst real-time load %.1f%% (normalised to 48k), silent patches %d/60\n", worstRt * 100.0, silent);
    CHECK (worstRt < 0.5);
}
