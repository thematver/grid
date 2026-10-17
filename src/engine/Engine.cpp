#include "Engine.h"
#include "ParamMaps.h"

#include <chrono>
#include <cmath>
#include <cstring>

namespace weft
{
using namespace dsp;

namespace
{
constexpr float kSmoothSeconds = 0.012f;
constexpr float kDepthSmoothSeconds = 0.02f;
constexpr float kStealFadeSamples = 96.0f;
constexpr float kFreezeFade = 0.28f;      // normalized FADE for lane freezes (~200 ms)

inline void fillRamp (float* out, float a, float b, int n) noexcept
{
    if (a == b) { for (int i = 0; i < n; ++i) out[i] = a; return; }
    const float step = (b - a) / (float) n;
    for (int i = 0; i < n; ++i) out[i] = a + step * (float) (i + 1);
}

inline float levelGain (float v) noexcept { v = clamp01 (v); return v * v * 1.5625f; }   // 0.8 -> unity

inline float softLimit (float x) noexcept
{
    const float a = std::abs (x);
    if (a <= 0.8f) return x;
    const float y = 0.8f + 0.2f * softClip ((a - 0.8f) * 5.0f) ;
    return x < 0.0f ? -std::min (y, 1.0f) : std::min (y, 1.0f);
}

const float kVoicePan[kMaxVoices] = { -0.5f, 0.5f, -0.25f, 0.25f, -0.75f, 0.75f, -1.0f, 1.0f };
} // namespace

Engine::Engine()
{
    current = &empty;
    globalNote.gate = false;
}

Engine::~Engine()
{
    if (current != &empty) delete current;
    delete pending.exchange (nullptr);
    Program* p = nullptr;
    while (retired.pop (p)) if (p != &empty) delete p;
}

void Engine::publish (std::unique_ptr<Program> p)
{
    Program* old = pending.exchange (p.release(), std::memory_order_acq_rel);
    delete old;   // never seen by the audio thread
}

void Engine::collectGarbage()
{
    Program* p = nullptr;
    while (retired.pop (p)) if (p != &empty) delete p;
}

void Engine::prepare (double sr, int)
{
    rate = sr > 0.0 ? sr : 48000.0;
    fRate = (float) rate;
    invRate = 1.0f / fRate;
    smoothCoef = 1.0f - std::exp (-(float) kBlock / (kSmoothSeconds * fRate));
    depthCoef = 1.0f - std::exp (-(float) kBlock / (kDepthSmoothSeconds * fRate));
    tele.sampleRate.store (fRate, std::memory_order_relaxed);

    for (auto& f : laneFreeze)
    {
        if (! f) f = createAudioModule (ModuleType::Freeze);
        if (f) { f->prepare (rate); f->reset(); }
    }
    if (current != &empty)
    {
        for (auto& c : current->keepCells) if (c && c->preparedRate != rate) c->prepare (rate);
        for (auto& s : current->keepSlots) if (s && s->preparedRate != rate) s->prepare (rate);
    }
    dcL.prepare (fRate); dcR.prepare (fRate);
    dcL.reset(); dcR.reset();
    allNotesOff (true);
    std::memset (laneOut, 0, sizeof (laneOut));
    std::memset (laneOutPrev, 0, sizeof (laneOutPrev));
    std::memset (laneMono, 0, sizeof (laneMono));
    std::memset (laneVoiceOut, 0, sizeof (laneVoiceOut));
    std::memset (cellVoiceOut, 0, sizeof (cellVoiceOut));
    std::memset (cellMono, 0, sizeof (cellMono));
    std::memset (cellSum, 0, sizeof (cellSum));
    firstBlock = true;
}

void Engine::applyMorph (ParamStore& ps, float x, float y)
{
    const auto w = morphWeights (x, y);
    float total = 0.0f;
    float ww[kNumScenes];
    for (int s = 0; s < kNumScenes; ++s)
    {
        ww[s] = ps.sceneStored[(size_t) s].load (std::memory_order_relaxed) ? w[(size_t) s] : 0.0f;
        total += ww[s];
    }
    if (total <= 1.0e-6f) return;
    for (auto& v : ww) v /= total;
    for (int i = 0; i < kNumMorphable; ++i)
    {
        float v = 0.0f;
        for (int s = 0; s < kNumScenes; ++s)
            if (ww[s] > 0.0f) v += ww[s] * ps.scene[(size_t) s][(size_t) i].load (std::memory_order_relaxed);
        ps.morphable (i).store (clamp01 (v), std::memory_order_relaxed);
    }
}

// ── MIDI & voices ──────────────────────────────────────────────────────────────────────────────

void Engine::handleMidi (const MidiEvent& e)
{
    const int type = e.status & 0xF0;
    switch (type)
    {
        case 0x90:
            if (e.data2 > 0) noteOn (e.data1 & 127, (float) e.data2 / 127.0f);
            else noteOff (e.data1 & 127);
            break;
        case 0x80: noteOff (e.data1 & 127); break;
        case 0xB0:
        {
            const int cc = e.data1 & 127;
            const float v = (float) (e.data2 & 127) / 127.0f;
            if (cc == 64)
            {
                const bool down = e.data2 >= 64;
                if (! down && sustainPedal)
                {
                    sustainPedal = false;
                    for (int i = 0; i < kMaxVoices; ++i)
                        if (voices[i].sustained) { voices[i].sustained = false; noteOff (-1 - i); }
                    if (noteStackSize == 0) noteOff (-1000);   // releases the global modulators
                }
                sustainPedal = down;
            }
            else if (cc == 1) wheelTarget = v;
            else if (cc == 120) allNotesOff (true);
            else if (cc == 123) allNotesOff (false);
            const int learn = store.learnMacro.load (std::memory_order_relaxed);
            if (learn >= 0 && learn < kNumMacros && cc != 64 && cc < 120)
            {
                store.macroCC[(size_t) learn].store (cc, std::memory_order_relaxed);
                store.learnedCC.store (cc, std::memory_order_relaxed);
                store.learnMacro.store (-1, std::memory_order_relaxed);
            }
            for (int m = 0; m < kNumMacros; ++m)
                if (store.macroCC[(size_t) m].load (std::memory_order_relaxed) == cc)
                    store.macro[(size_t) m].store (v, std::memory_order_relaxed);
            break;
        }
        case 0xE0:
            bend = clampf ((float) (((e.data2 & 127) << 7) | (e.data1 & 127)) / 8192.0f - 1.0f, -1.0f, 1.0f);
            break;
        case 0xD0: pressureTarget = (float) (e.data1 & 127) / 127.0f; break;
        case 0xA0:
            for (auto& v : voices)
                if (v.active && v.note == (e.data1 & 127)) v.pressure = (float) (e.data2 & 127) / 127.0f;
            break;
        default: break;
    }
}

void Engine::noteOn (int note, float velocity)
{
    // note stack (for mono / legato)
    for (int i = 0; i < noteStackSize; ++i)
        if (noteStack[i] == note) { std::memmove (noteStack + i, noteStack + i + 1, sizeof (int) * (size_t) (noteStackSize - i - 1)); --noteStackSize; break; }
    if (noteStackSize < 128) noteStack[noteStackSize++] = note;
    heldCount = noteStackSize;
    lastVelocity = velocity;
    lastTarget = (float) note;
    if (store.glide.load (std::memory_order_relaxed) <= 0.0f) lastPitch = (float) note;
    anyTrigger = true;
    globalNote.trigger = 0;

    for (int s = 0; s < kNumSlots; ++s)
        if (auto* si = current->slots[s].inst; si && si->global)
        {
            NoteState ns = globalNote; ns.pitch = (float) note; ns.velocity = velocity; ns.gate = true; ns.trigger = 0;
            si->global->noteOn (ns);
        }
    for (int c = 0; c < kNumCells; ++c)
        if (auto* ci = current->cells[c].inst; ci && ci->global && ! current->cells[c].voiced)
        {
            NoteState ns = globalNote; ns.pitch = (float) note; ns.velocity = velocity; ns.gate = true; ns.trigger = 0;
            ci->global->noteOn (ns);
        }

    const int poly = std::max (1, std::min (kMaxVoices, store.polyphony.load (std::memory_order_relaxed)));
    if (poly == 1)
    {
        Voice& v = voices[0];
        if (v.active && v.gate && ! v.stealing)
        {
            v.note = note; v.targetPitch = (float) note; v.velocity = velocity;   // legato
            if (store.glide.load (std::memory_order_relaxed) <= 0.0f) v.pitch = (float) note;
        }
        else startVoice (0, note, velocity);
        for (int i = 1; i < kMaxVoices; ++i) if (voices[i].active) { voices[i].gate = false; voices[i].envStage = 3; }
        return;
    }

    int chosen = -1;
    for (int i = 0; i < poly; ++i) if (voices[i].active && voices[i].note == note && ! voices[i].stealing) { chosen = i; break; }
    if (chosen < 0) for (int i = 0; i < poly; ++i) if (! voices[i].active) { chosen = i; break; }
    if (chosen < 0)
    {
        float best = 1.0e9f;
        for (int i = 0; i < poly; ++i)   // released voices first, quietest wins
            if (! voices[i].gate && voices[i].env < best && ! voices[i].stealing) { best = voices[i].env; chosen = i; }
        if (chosen < 0)
        {
            uint32_t oldest = 0xffffffffu;
            for (int i = 0; i < poly; ++i) if (voices[i].age < oldest) { oldest = voices[i].age; chosen = i; }
        }
    }
    if (chosen < 0) return;
    Voice& v = voices[chosen];
    if (v.active && v.env > 1.0e-3f)
    {
        v.stealing = true;
        v.stealNote = note;
        v.stealVelocity = velocity;
        if (v.stealGain <= 0.0f || v.stealGain > 1.0f) v.stealGain = 1.0f;
    }
    else startVoice (chosen, note, velocity);
}

void Engine::startVoice (int vi, int note, float velocity)
{
    Voice& v = voices[vi];
    const bool wasActive = v.active;
    const float glide = store.glide.load (std::memory_order_relaxed);
    v.active = true; v.gate = true; v.sustained = false; v.stealing = false; v.stealGain = 1.0f;
    v.note = note; v.velocity = velocity; v.age = ++ageCounter;
    v.targetPitch = (float) note;
    v.pitch = glide > 0.0f ? lastPitch : (float) note;
    if (! wasActive) v.env = 0.0f;
    v.envStage = 1;
    v.pressure = 0.0f;
    v.state.pitch = v.pitch; v.state.velocity = velocity; v.state.gate = true; v.state.trigger = 0; v.state.release = -1;

    const float spread = clamp01 (store.spread.load (std::memory_order_relaxed));
    const float pan = kVoicePan[vi] * spread;
    const float ang = (pan + 1.0f) * 0.25f * kPi;
    v.panL = std::cos (ang); v.panR = std::sin (ang);

    for (int c = 0; c < kNumCells; ++c)
    {
        const auto& cp = current->cells[c];
        if (! cp.voiced || ! cp.inst || ! cp.inst->voice[(size_t) vi]) continue;
        if (! wasActive) cp.inst->voice[(size_t) vi]->reset();
        cp.inst->voice[(size_t) vi]->noteOn (v.state);
    }
    for (int s = 0; s < kNumSlots; ++s)
    {
        auto* si = current->slots[s].inst;
        if (! si || ! si->voice[(size_t) vi]) continue;
        if (! wasActive) si->voice[(size_t) vi]->reset();
        si->voice[(size_t) vi]->noteOn (v.state);
        if (! wasActive) slotVoicePrev[s][vi] = slotVoiceOut[s][vi] = 0.0f;
    }
    newestVoice = vi;
}

void Engine::noteOff (int note)
{
    // note < 0 encodes "release voice (-1 - note)" after the sustain pedal is lifted
    if (note >= 0)
    {
        for (int i = 0; i < noteStackSize; ++i)
            if (noteStack[i] == note) { std::memmove (noteStack + i, noteStack + i + 1, sizeof (int) * (size_t) (noteStackSize - i - 1)); --noteStackSize; break; }
        heldCount = noteStackSize;
    }

    auto release = [this] (int vi)
    {
        Voice& v = voices[vi];
        v.gate = false;
        v.envStage = 3;
        v.state.release = 0;
        for (int c = 0; c < kNumCells; ++c)
        {
            const auto& cp = current->cells[c];
            if (cp.voiced && cp.inst && cp.inst->voice[(size_t) vi]) cp.inst->voice[(size_t) vi]->noteOff();
        }
        for (int s = 0; s < kNumSlots; ++s)
            if (auto* si = current->slots[s].inst; si && si->voice[(size_t) vi]) si->voice[(size_t) vi]->noteOff();
    };

    if (note < 0)
    {
        const int vi = -1 - note;   // out-of-range (e.g. -1000) only runs the global release below
        if (vi >= 0 && vi < kMaxVoices && voices[vi].active)
        {
            bool held = false;
            for (int i = 0; i < noteStackSize; ++i) if (noteStack[i] == voices[vi].note) held = true;
            if (! held) release (vi);
        }
    }
    else
    {
        const int poly = std::max (1, std::min (kMaxVoices, store.polyphony.load (std::memory_order_relaxed)));
        if (poly == 1)
        {
            Voice& v = voices[0];
            if (v.active && v.gate && v.note == note)
            {
                if (noteStackSize > 0)
                {
                    const int next = noteStack[noteStackSize - 1];
                    v.note = next; v.targetPitch = (float) next; lastTarget = (float) next;
                    if (store.glide.load (std::memory_order_relaxed) <= 0.0f) v.pitch = (float) next;
                }
                else if (sustainPedal) v.sustained = true;
                else release (0);
            }
        }
        else
        {
            for (int i = 0; i < kMaxVoices; ++i)
            {
                Voice& v = voices[i];
                if (v.active && v.gate && v.note == note && ! v.stealing)
                {
                    if (sustainPedal) v.sustained = true;
                    else release (i);
                }
                if (v.stealing && v.stealNote == note) v.stealNote = -1;   // released before the steal completed
            }
        }
    }

    if (noteStackSize == 0 && ! sustainPedal)
    {
        globalNote.release = 0;
        for (int s = 0; s < kNumSlots; ++s)
            if (auto* si = current->slots[s].inst; si && si->global) si->global->noteOff();
        for (int c = 0; c < kNumCells; ++c)
            if (auto* ci = current->cells[c].inst; ci && ci->global && ! current->cells[c].voiced) ci->global->noteOff();
    }
}

void Engine::allNotesOff (bool hard)
{
    noteStackSize = 0;
    heldCount = 0;
    sustainPedal = false;
    for (auto& v : voices)
    {
        if (hard) { v = Voice(); continue; }
        if (v.active) { v.gate = false; v.envStage = 3; v.sustained = false; }
    }
    globalNote.gate = false;
}

void Engine::panic()
{
    allNotesOff (true);
    for (auto& c : current->keepCells)
    {
        if (! c) continue;
        if (c->global) c->global->reset();
        for (auto& v : c->voice) if (v) v->reset();
    }
    for (auto& s : current->keepSlots)
    {
        if (! s) continue;
        if (s->global) s->global->reset();
        for (auto& v : s->voice) if (v) v->reset();
    }
    for (auto& f : laneFreeze) if (f) f->reset();
    std::memset (laneOut, 0, sizeof (laneOut));
    std::memset (laneOutPrev, 0, sizeof (laneOutPrev));
    std::memset (laneVoiceOut, 0, sizeof (laneVoiceOut));
    std::memset (cellVoiceOut, 0, sizeof (cellVoiceOut));
    std::memset (cellMono, 0, sizeof (cellMono));
    std::memset (cellSum, 0, sizeof (cellSum));
    dcL.reset(); dcR.reset();
}

void Engine::updateVoices (int n)
{
    const float attack = std::max (0.0005f, store.attack.load (std::memory_order_relaxed));
    const float releaseT = std::max (0.002f, store.release.load (std::memory_order_relaxed));
    const float glide = std::max (0.0f, store.glide.load (std::memory_order_relaxed));
    const float bendSemis = bend * (float) std::max (0, std::min (48, store.bendRange.load (std::memory_order_relaxed)));
    const float attackStep = 1.0f / (attack * fRate);
    const float relCoef = std::exp (-6.9f / (releaseT * fRate));   // -60 dB after `release` seconds
    const float glideCoef = glide > 0.0f ? 1.0f - std::exp (-(float) n / (glide * fRate)) : 1.0f;

    int active = 0;
    for (int vi = 0; vi < kMaxVoices; ++vi)
    {
        Voice& v = voices[vi];
        if (! v.active) continue;
        ++active;
        v.pitch += (v.targetPitch - v.pitch) * glideCoef;
        v.state.pitch = v.pitch + bendSemis;
        v.state.velocity = v.velocity;
        v.state.gate = v.gate;

        float e = v.env;
        for (int i = 0; i < n; ++i)
        {
            if (v.envStage == 1) { e += attackStep; if (e >= 1.0f) { e = 1.0f; v.envStage = 2; } }
            else if (v.envStage == 3) { e *= relCoef; }
            float g = e;
            if (v.stealing)
            {
                v.stealGain -= 1.0f / kStealFadeSamples;
                g *= std::max (0.0f, v.stealGain);
            }
            v.envBuf[i] = g;
        }
        v.env = e;
        if (v.envStage == 3 && e < 1.0e-4f && ! v.stealing) { v.active = false; v.env = 0.0f; v.envStage = 0; }
    }
    // glide of the global pitch
    lastPitch += (lastTarget - lastPitch) * glideCoef;
    globalNote.pitch = lastPitch + bendSemis;
    globalNote.velocity = lastVelocity;
    globalNote.gate = heldCount > 0 || sustainPedal;
    tele.activeVoices.store (active, std::memory_order_relaxed);
    tele.lastNote.store (lastTarget, std::memory_order_relaxed);
}

// ── modulation ─────────────────────────────────────────────────────────────────────────────────

float Engine::controlSource (const ModInput& in, int voice, bool end) const
{
    switch (in.kind)
    {
        case SourceKind::Slot:
            if (in.index >= kNumSlots) return 0.0f;
            if (voice >= 0 && slotVoiceActive[in.index])
                return end ? slotVoiceOut[in.index][voice] : slotVoicePrev[in.index][voice];
            return end ? slotOut[in.index] : slotOutPrev[in.index];
        case SourceKind::Macro:
            return in.index < kNumMacros ? (end ? macroCur[in.index] : macroPrev[in.index]) : 0.0f;
        case SourceKind::Midi:
            switch ((MidiSource) in.index)
            {
                case MidiSource::Velocity: return voice >= 0 ? voices[voice].velocity : lastVelocity;
                case MidiSource::Key:      return (voice >= 0 ? (float) voices[voice].note : lastTarget) / 127.0f;
                case MidiSource::Wheel:    return end ? wheel : wheelPrev;
                case MidiSource::Pressure: return std::max (voice >= 0 ? voices[voice].pressure : 0.0f, end ? pressure : pressurePrev);
            }
            return 0.0f;
        case SourceKind::Audio: return 0.0f;
    }
    return 0.0f;
}

const float* Engine::audioSource (int cell, int voice) const
{
    if (cell < 0 || cell >= kNumCells) return zeros;
    const auto& cp = current->cells[cell];
    if (cp.type == ModuleType::None) return zeros;
    if (voice >= 0) return cp.voiced ? cellVoiceOut[cell][voice] : cellMono[cell];
    return cp.voiced ? cellSum[cell] : cellMono[cell];
}

void Engine::buildParams (const ParamRouting& r, bool isSlot, int base, int voice, int n, float* out)
{
    float b0 = isSlot ? slotPrev[base] : cellPrev[base];
    float b1 = isSlot ? slotCur[base] : cellCur[base];
    for (int i = 0; i < r.count; ++i)
    {
        const auto& in = r.in[i];
        b0 += depthPrev[in.connId] * controlSource (in, voice, false);
        b1 += depthCur[in.connId] * controlSource (in, voice, true);
    }
    if (r.audioCount == 0)
    {
        fillRamp (out, clamp01 (b0), clamp01 (b1), n);
    }
    else
    {
        fillRamp (out, b0, b1, n);
        for (int j = 0; j < r.audioCount; ++j)
        {
            const auto& in = r.in[r.count + j];
            const float* src = audioSource (in.index, voice);
            const float d = depthCur[in.connId];
            for (int i = 0; i < n; ++i) out[i] += d * src[i];
        }
        for (int i = 0; i < n; ++i) out[i] = clamp01 (out[i]);
    }
    if (voice < 0 || voice == newestVoice)
    {
        auto& live = isSlot ? tele.slotLive[(size_t) base] : tele.cellLive[(size_t) base];
        live.store (out[n - 1], std::memory_order_relaxed);
    }
}

void Engine::evalSlots (int n)
{
    float pbuf[kNumParams][kBlock];
    const float* lanePtrs[kNumLanes] = { laneMono[0], laneMono[1], laneMono[2], laneMono[3] };
    ++traceCounter;
    const bool writeTrace = traceCounter >= Telemetry::kTraceDecimation;
    if (writeTrace) traceCounter = 0;

    for (int s = 0; s < kNumSlots; ++s)
    {
        const auto& sp = current->slots[s];
        slotOutPrev[s] = slotOut[s];
        if (sp.type == ModuleType::None || ! sp.inst || ! sp.inst->global)
        {
            slotOut[s] = 0.0f;
            slotVoiceActive[s] = false;
            continue;
        }
        float aux[kAuxSize];
        for (int i = 0; i < kAuxSize; ++i) aux[i] = store.slotAux[(size_t) (s * kAuxSize + i)].load (std::memory_order_relaxed);

        ParamInputs pi;
        for (int m = 0; m < kNumModes; ++m) pi.mode[m] = store.slotMode[(size_t) (s * kNumModes + m)].load (std::memory_order_relaxed);
        for (int k = 0; k < kNumParams; ++k) { buildParams (sp.routes[k], true, s * kNumParams + k, -1, n, pbuf[k]); pi.v[k] = pbuf[k]; }

        ControlContext cx;
        cx.numSamples = n; cx.sampleRate = fRate; cx.voice = -1; cx.note = &globalNote; cx.transport = &transport;
        cx.aux = aux; cx.slotValues = slotOut; cx.macroValues = macroCur;
        cx.laneSignal = lanePtrs; cx.laneSignalLength = n; cx.sidechain = sideMono;

        slotOut[s] = clamp01 (sanitize (sp.inst->global->process (cx, pi)));

        const bool perVoice = sp.inst->voice[0] != nullptr && (sp.type == ModuleType::Env || (sp.type == ModuleType::Lfo && pi.mode[1] == 2));
        slotVoiceActive[s] = perVoice;
        if (perVoice)
        {
            for (int vi = 0; vi < kMaxVoices; ++vi)
            {
                if (! voices[vi].active) continue;
                ParamInputs pv;
                pv.mode[0] = pi.mode[0]; pv.mode[1] = pi.mode[1];
                for (int k = 0; k < kNumParams; ++k) { buildParams (sp.routes[k], true, s * kNumParams + k, vi, n, pbuf[k]); pv.v[k] = pbuf[k]; }
                cx.voice = vi; cx.note = &voices[vi].state;
                slotVoicePrev[s][vi] = slotVoiceOut[s][vi];
                slotVoiceOut[s][vi] = clamp01 (sanitize (sp.inst->voice[(size_t) vi]->process (cx, pv)));
            }
            cx.voice = -1; cx.note = &globalNote;
        }

        // telemetry: per-voice sources show the newest voice
        float shown = slotOut[s];
        if (perVoice && newestVoice >= 0 && voices[newestVoice].active) shown = slotVoiceOut[s][newestVoice];
        tele.slotValue[(size_t) s].store (shown, std::memory_order_relaxed);
        tele.slotPhase[(size_t) s].store (sp.inst->global->phase(), std::memory_order_relaxed);
        if (writeTrace)
        {
            const auto w = tele.traceWrite[(size_t) s].load (std::memory_order_relaxed);
            tele.trace[(size_t) s][w % Telemetry::kTraceSize].store (shown, std::memory_order_relaxed);
            tele.traceWrite[(size_t) s].store (w + 1, std::memory_order_relaxed);
        }
    }
}

void Engine::writeScope (int cell, const float* x, int n)
{
    auto& ring = tele.scope[(size_t) cell];
    auto w = tele.scopeWrite[(size_t) cell].load (std::memory_order_relaxed);
    float pk = 0.0f;
    for (int i = 0; i < n; ++i)
    {
        ring[(w + (uint32_t) i) % Telemetry::kScopeSize].store (x[i], std::memory_order_relaxed);
        pk = std::max (pk, std::abs (x[i]));
    }
    tele.scopeWrite[(size_t) cell].store (w + (uint32_t) n, std::memory_order_relaxed);
    cellPeakState[cell] = std::max (pk, cellPeakState[cell] * 0.97f);
    tele.cellPeak[(size_t) cell].store (cellPeakState[cell], std::memory_order_relaxed);
}

// ── lanes ──────────────────────────────────────────────────────────────────────────────────────

void Engine::evalLanes (const float* const* sidechain, int numSide, int n)
{
    float pbuf[kNumParams][kBlock];
    float vbuf[kBlock];
    int activeIdx[kMaxVoices];
    int nActive = 0;
    for (int vi = 0; vi < kMaxVoices; ++vi) if (voices[vi].active) activeIdx[nActive++] = vi;

    bool anySolo = false;
    for (int l = 0; l < kNumLanes; ++l) anySolo |= store.laneSolo[(size_t) l].load (std::memory_order_relaxed) != 0;

    for (int c = 0; c < kNumCells; ++c) cellSumFresh[c] = false;

    for (int l = 0; l < kNumLanes; ++l)
    {
        const LaneProg& lp = current->lanes[l];
        float* L = laneOut[l][0];
        float* R = laneOut[l][1];
        std::memset (L, 0, sizeof (float) * (size_t) n);
        std::memset (R, 0, sizeof (float) * (size_t) n);

        const bool entryVoiced = lp.voiced && (lp.entry < 0 || lp.entry < lp.voicedCount);

        const float* laneInG[kNumLanes * 2] {};
        const float* laneInV[kNumLanes * 2] {};
        bool perVoiceSrc[kNumLanes] {};
        for (int s = 0; s < kNumLanes; ++s)
        {
            if (! (lp.inboundMask & (1u << s))) continue;
            const bool forward = s < l;
            const float* srcL = forward ? laneOut[s][0] : laneOutPrev[s][0];
            const float* srcR = forward ? laneOut[s][1] : laneOutPrev[s][1];
            perVoiceSrc[s] = forward && current->lanes[s].outputPerVoice && entryVoiced;
            if (! perVoiceSrc[s])
            {
                laneInG[s * 2] = srcL; laneInG[s * 2 + 1] = srcR;
                if (entryVoiced && nActive > 0)
                {
                    const float k = 0.5f / (float) nActive;
                    for (int i = 0; i < n; ++i) scratchIn[s][i] = (srcL[i] + srcR[i]) * k;
                }
            }
        }

        // ---- per-voice region
        if (lp.voiced)
        {
            for (int a = 0; a < nActive; ++a)
            {
                const int vi = activeIdx[a];
                Voice& voice = voices[vi];
                for (int s = 0; s < kNumLanes; ++s)
                {
                    if (! (lp.inboundMask & (1u << s))) { laneInV[s * 2] = laneInV[s * 2 + 1] = nullptr; continue; }
                    const float* p = perVoiceSrc[s] ? laneVoiceOut[s][vi] : scratchIn[s];
                    laneInV[s * 2] = laneInV[s * 2 + 1] = p;
                }
                if (lp.entry < 0)
                {
                    std::memset (vbuf, 0, sizeof (float) * (size_t) n);
                    for (int s = 0; s < kNumLanes; ++s)
                        if (laneInV[s * 2]) for (int i = 0; i < n; ++i) vbuf[i] += laneInV[s * 2][i];
                }
                else std::memset (vbuf, 0, sizeof (float) * (size_t) n);

                for (int ci = 0; ci < lp.voicedCount; ++ci)
                {
                    const int c = lp.cells[ci];
                    const CellProg& cp = current->cells[c];
                    AudioModule* mod = cp.inst ? cp.inst->voice[(size_t) vi].get() : nullptr;
                    if (! mod) continue;
                    float aux[kAuxSize];
                    for (int i = 0; i < kAuxSize; ++i) aux[i] = store.cellAux[(size_t) (c * kAuxSize + i)].load (std::memory_order_relaxed);
                    ParamInputs pi;
                    for (int m = 0; m < kNumModes; ++m) pi.mode[m] = store.cellMode[(size_t) (c * kNumModes + m)].load (std::memory_order_relaxed);
                    for (int k = 0; k < kNumParams; ++k) { buildParams (cp.routes[k], false, c * kNumParams + k, vi, n, pbuf[k]); pi.v[k] = pbuf[k]; }

                    ProcessContext cx;
                    cx.numSamples = n; cx.sampleRate = fRate; cx.invSampleRate = invRate; cx.voice = vi;
                    cx.note = &voice.state; cx.transport = &transport; cx.aux = aux; cx.resource = cp.resource;
                    cx.laneIn = cp.type == ModuleType::In ? laneInV : nullptr;
                    cx.inboundMask = cp.inboundMask;
                    cx.sidechain[0] = numSide > 0 ? sidechain[0] : nullptr;
                    cx.sidechain[1] = numSide > 1 ? sidechain[1] : cx.sidechain[0];

                    float* io[2] = { vbuf, nullptr };
                    mod->process (cx, pi, io, 1);
                    sanitizeBuffer (vbuf, n);

                    if (cp.audioSource) std::memcpy (cellVoiceOut[c][vi], vbuf, sizeof (float) * (size_t) n);
                    if (! cellSumFresh[c]) { std::memcpy (cellSum[c], vbuf, sizeof (float) * (size_t) n); cellSumFresh[c] = true; }
                    else for (int i = 0; i < n; ++i) cellSum[c][i] += vbuf[i];
                }
                for (int i = 0; i < n; ++i) vbuf[i] *= voice.envBuf[i];
                if (lp.outputPerVoice) std::memcpy (laneVoiceOut[l][vi], vbuf, sizeof (float) * (size_t) n);
                for (int i = 0; i < n; ++i) { L[i] += vbuf[i] * voice.panL; R[i] += vbuf[i] * voice.panR; }
            }
            for (int ci = 0; ci < lp.voicedCount; ++ci)
            {
                const int c = lp.cells[ci];
                if (! cellSumFresh[c]) { std::memset (cellSum[c], 0, sizeof (float) * (size_t) n); cellSumFresh[c] = true; }
                writeScope (c, cellSum[c], n);
            }
        }
        else if (lp.entry < 0)
        {
            for (int s = 0; s < kNumLanes; ++s)
                if (laneInG[s * 2])
                    for (int i = 0; i < n; ++i) { L[i] += laneInG[s * 2][i]; R[i] += laneInG[s * 2 + 1][i]; }
        }

        // ---- global region
        for (int ci = lp.voicedCount; ci < lp.numCells; ++ci)
        {
            const int c = lp.cells[ci];
            const CellProg& cp = current->cells[c];
            AudioModule* mod = cp.inst ? cp.inst->global.get() : nullptr;
            if (! mod) continue;
            float aux[kAuxSize];
            for (int i = 0; i < kAuxSize; ++i) aux[i] = store.cellAux[(size_t) (c * kAuxSize + i)].load (std::memory_order_relaxed);
            ParamInputs pi;
            for (int m = 0; m < kNumModes; ++m) pi.mode[m] = store.cellMode[(size_t) (c * kNumModes + m)].load (std::memory_order_relaxed);
            for (int k = 0; k < kNumParams; ++k) { buildParams (cp.routes[k], false, c * kNumParams + k, -1, n, pbuf[k]); pi.v[k] = pbuf[k]; }

            ProcessContext cx;
            cx.numSamples = n; cx.sampleRate = fRate; cx.invSampleRate = invRate; cx.voice = -1;
            cx.note = &globalNote; cx.transport = &transport; cx.aux = aux; cx.resource = cp.resource;
            cx.laneIn = cp.type == ModuleType::In ? laneInG : nullptr;
            cx.inboundMask = cp.inboundMask;
            cx.sidechain[0] = numSide > 0 ? sidechain[0] : nullptr;
            cx.sidechain[1] = numSide > 1 ? sidechain[1] : cx.sidechain[0];

            float* io[2] = { L, R };
            mod->process (cx, pi, io, 2);
            sanitizeBuffer (L, n);
            sanitizeBuffer (R, n);
            for (int i = 0; i < n; ++i) cellMono[c][i] = 0.5f * (L[i] + R[i]);
            writeScope (c, cellMono[c], n);
        }

        // ---- lane freeze (always running so it has recent audio to hold)
        if (auto* fz = laneFreeze[l].get())
        {
            const bool on = store.laneFreeze[(size_t) l].load (std::memory_order_relaxed) != 0;
            float hold[kBlock], fade[kBlock], size[kBlock], mix[kBlock];
            for (int i = 0; i < n; ++i) { hold[i] = on ? 1.0f : 0.0f; fade[i] = kFreezeFade; size[i] = 0.62f; mix[i] = 1.0f; }
            ParamInputs pi; pi.v[0] = hold; pi.v[1] = fade; pi.v[2] = size; pi.v[3] = mix;
            float aux[kAuxSize] {};
            ProcessContext cx;
            cx.numSamples = n; cx.sampleRate = fRate; cx.invSampleRate = invRate; cx.voice = -1;
            cx.note = &globalNote; cx.transport = &transport; cx.aux = aux;
            float* io[2] = { L, R };
            fz->process (cx, pi, io, 2);
            sanitizeBuffer (L, n);
            sanitizeBuffer (R, n);
        }

        // ---- level, mute, solo
        const float g0 = laneGainPrev[l], g1 = laneGainCur[l];
        const bool muted = store.laneMute[(size_t) l].load (std::memory_order_relaxed) != 0
                        || (anySolo && store.laneSolo[(size_t) l].load (std::memory_order_relaxed) == 0);
        (void) muted;   // folded into laneGainCur by the smoother
        float pk = 0.0f;
        const float step = (g1 - g0) / (float) n;
        for (int i = 0; i < n; ++i)
        {
            const float g = g0 + step * (float) (i + 1);
            L[i] *= g; R[i] *= g;
            laneMono[l][i] = 0.5f * (L[i] + R[i]);
            pk = std::max (pk, std::max (std::abs (L[i]), std::abs (R[i])));
        }
        meterPeak[l] = std::max (pk, meterPeak[l] * 0.94f);
        tele.lanePeak[(size_t) l].store (meterPeak[l], std::memory_order_relaxed);
    }
}

// ── main loop ──────────────────────────────────────────────────────────────────────────────────

void Engine::renderSub (float* const* out, int numOut, const float* const* sidechain, int numSide, int offset, int n)
{
    // transport
    transport.ppq = freePpq;
    freePpq += (double) n / (double) transport.samplesPerBeat;

    // smoothing targets
    bool anySolo = false;
    for (int l = 0; l < kNumLanes; ++l) anySolo |= store.laneSolo[(size_t) l].load (std::memory_order_relaxed) != 0;

    for (int c = 0; c < kNumCells; ++c)
    {
        const void* inst = current->cells[c].inst;
        const bool snap = firstBlock || inst != lastCellInst[c];
        lastCellInst[c] = inst;
        for (int k = 0; k < kNumParams; ++k)
        {
            const int i = c * kNumParams + k;
            const float t = clamp01 (store.cell[(size_t) i].load (std::memory_order_relaxed));
            cellPrev[i] = snap ? t : cellCur[i];
            cellCur[i] = snap ? t : cellCur[i] + (t - cellCur[i]) * smoothCoef;
        }
    }
    for (int s = 0; s < kNumSlots; ++s)
    {
        const void* inst = current->slots[s].inst;
        const bool snap = firstBlock || inst != lastSlotInst[s];
        lastSlotInst[s] = inst;
        for (int k = 0; k < kNumParams; ++k)
        {
            const int i = s * kNumParams + k;
            const float t = clamp01 (store.slot[(size_t) i].load (std::memory_order_relaxed));
            slotPrev[i] = snap ? t : slotCur[i];
            slotCur[i] = snap ? t : slotCur[i] + (t - slotCur[i]) * smoothCoef;
        }
    }
    for (int m = 0; m < kNumMacros; ++m)
    {
        const float t = clamp01 (store.macro[(size_t) m].load (std::memory_order_relaxed));
        macroPrev[m] = firstBlock ? t : macroCur[m];
        macroCur[m] = firstBlock ? t : macroCur[m] + (t - macroCur[m]) * smoothCoef;
    }
    for (int i = 0; i < kMaxConnections; ++i)
    {
        const float t = clampf (store.depth[(size_t) i].load (std::memory_order_relaxed), -1.0f, 1.0f);
        depthPrev[i] = firstBlock ? t : depthCur[i];
        depthCur[i] = firstBlock ? t : depthCur[i] + (t - depthCur[i]) * depthCoef;
    }
    for (int l = 0; l < kNumLanes; ++l)
    {
        const bool muted = store.laneMute[(size_t) l].load (std::memory_order_relaxed) != 0
                        || (anySolo && store.laneSolo[(size_t) l].load (std::memory_order_relaxed) == 0);
        const float t = muted ? 0.0f : levelGain (store.laneLevel[(size_t) l].load (std::memory_order_relaxed));
        laneGainPrev[l] = firstBlock ? t : laneGainCur[l];
        laneGainCur[l] = firstBlock ? t : laneGainCur[l] + (t - laneGainCur[l]) * smoothCoef;
    }
    {
        const float t = levelGain (store.master.load (std::memory_order_relaxed));
        masterPrev = firstBlock ? t : masterCur;
        masterCur = firstBlock ? t : masterCur + (t - masterCur) * smoothCoef;
    }
    wheelPrev = wheel; wheel += (wheelTarget - wheel) * smoothCoef;
    pressurePrev = pressure; pressure += (pressureTarget - pressure) * smoothCoef;
    firstBlock = false;

    // side-chain mono (FOLLOW "IN")
    const float* side[2] = { nullptr, nullptr };
    if (numSide > 0 && sidechain && sidechain[0])
    {
        side[0] = sidechain[0] + offset;
        side[1] = (numSide > 1 && sidechain[1]) ? sidechain[1] + offset : side[0];
        for (int i = 0; i < n; ++i) sideMono[i] = 0.5f * (side[0][i] + side[1][i]);
    }
    else std::memset (sideMono, 0, sizeof (sideMono));

    updateVoices (n);
    evalSlots (n);
    evalLanes (side, side[0] ? 2 : 0, n);

    // finish voice steals: the old note has faded out, start the new one
    for (int vi = 0; vi < kMaxVoices; ++vi)
    {
        Voice& v = voices[vi];
        if (v.stealing && v.stealGain <= 0.0f)
        {
            const int note = v.stealNote;
            const float vel = v.stealVelocity;
            v.stealing = false;
            v.active = false;
            v.env = 0.0f;
            if (note >= 0) startVoice (vi, note, vel);
        }
    }
    globalNote.trigger = -1;
    globalNote.release = -1;
    for (auto& v : voices) { v.state.trigger = -1; v.state.release = -1; }

    // master
    float* oL = out[0] + offset;
    float* oR = numOut > 1 ? out[1] + offset : nullptr;
    const float step = (masterCur - masterPrev) / (float) n;
    float pkL = 0.0f, pkR = 0.0f;
    for (int i = 0; i < n; ++i)
    {
        float l = 0.0f, r = 0.0f;
        for (int ln = 0; ln < kNumLanes; ++ln)
            if (current->lanes[ln].toMix) { l += laneOut[ln][0][i]; r += laneOut[ln][1][i]; }
        const float g = masterPrev + step * (float) (i + 1);
        l = softLimit (dcL.process (l * g));
        r = softLimit (dcR.process (r * g));
        if (! std::isfinite (l)) { l = 0.0f; dcL.reset(); }
        if (! std::isfinite (r)) { r = 0.0f; dcR.reset(); }
        pkL = std::max (pkL, std::abs (l));
        pkR = std::max (pkR, std::abs (r));
        if (oR) { oL[i] = l; oR[i] = r; }
        else oL[i] = 0.5f * (l + r);
    }
    masterPeak[0] = std::max (pkL, masterPeak[0] * 0.94f);
    masterPeak[1] = std::max (pkR, masterPeak[1] * 0.94f);
    tele.masterPeakL.store (masterPeak[0], std::memory_order_relaxed);
    tele.masterPeakR.store (masterPeak[1], std::memory_order_relaxed);

    std::memcpy (laneOutPrev, laneOut, sizeof (laneOut));
}

void Engine::process (float* const* out, int numOut, const float* const* sidechain, int numSide,
                      int numSamples, const MidiEvent* events, int numEvents, const HostTransport& ht)
{
    ScopedFlushDenormals ftz;
    const auto t0 = std::chrono::steady_clock::now();

    if (retired.hasSpace())
        if (Program* p = pending.exchange (nullptr, std::memory_order_acq_rel))
        {
            retired.push (current);
            current = p;
        }

    if (store.panicRequest.exchange (0, std::memory_order_relaxed) != 0) panic();

    {
        const float mx = store.morphX.load (std::memory_order_relaxed);
        const float my = store.morphY.load (std::memory_order_relaxed);
        const uint32_t sv = store.sceneVersion.load (std::memory_order_relaxed);
        if (mx != lastMorphX.load (std::memory_order_relaxed) || my != lastMorphY.load (std::memory_order_relaxed) || sv != lastSceneVersion)
        {
            applyMorph (store, mx, my);
            lastMorphX.store (mx, std::memory_order_relaxed);
            lastMorphY.store (my, std::memory_order_relaxed);
            lastSceneVersion = sv;
        }
    }

    if (ht.valid && ht.bpm > 1.0)
    {
        transport.bpm = clampf ((float) ht.bpm, 20.0f, 999.0f);
        transport.playing = ht.playing;
        if (ht.playing) freePpq = ht.ppq;
    }
    else transport.playing = false;
    transport.samplesPerBeat = (float) (rate * 60.0 / transport.bpm);
    tele.bpm.store ((float) transport.bpm, std::memory_order_relaxed);
    tele.hostPlaying.store (transport.playing ? 1 : 0, std::memory_order_relaxed);

    int pos = 0, ev = 0;
    while (pos < numSamples)
    {
        while (ev < numEvents && events[ev].offset <= pos) handleMidi (events[ev++]);
        int end = std::min (pos + kBlock, numSamples);
        if (ev < numEvents && events[ev].offset < end) end = std::max (pos + 1, events[ev].offset);
        renderSub (out, numOut, sidechain, numSide, pos, end - pos);
        pos = end;
    }
    while (ev < numEvents) handleMidi (events[ev++]);

    const auto t1 = std::chrono::steady_clock::now();
    const double used = std::chrono::duration<double> (t1 - t0).count();
    const double avail = (double) numSamples / rate;
    if (avail > 0.0)
    {
        cpuSmoothed += ((float) (used / avail) - cpuSmoothed) * 0.05f;
        tele.cpu.store (cpuSmoothed, std::memory_order_relaxed);
    }
    tele.blocks.fetch_add (1, std::memory_order_relaxed);
}

} // namespace weft
