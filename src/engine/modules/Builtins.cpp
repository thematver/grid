// Built-in resources, generated procedurally so the plug-in ships without audio files:
// six wavetables for WAVE and six samples for GRAIN. Message thread only (everything allocates).
#include "../Builtins.h"
#include "SourceDsp.h"

#include <algorithm>
#include <cmath>
#include <complex>
#include <functional>
#include <map>
#include <mutex>

namespace weft
{
namespace
{
using cd = std::complex<double>;
constexpr int N = AudioResource::kWaveFrameSize;
constexpr int kHarm = N / 2;               // harmonics stored at level 0
constexpr double kPi = srcdsp::kPiD;
constexpr int kTableFrames = 32;

std::mutex& cacheMutex() { static std::mutex m; return m; }

/** Spectrum of one cycle: amplitude and phase (radians, sine phase) for harmonics 1..kHarm. */
struct Spectrum
{
    std::vector<double> amp = std::vector<double> (kHarm + 1, 0.0);
    std::vector<double> phase = std::vector<double> (kHarm + 1, 0.0);
};

/** Analyses one oversampled cycle (length M, power of two >= N) into a spectrum. */
Spectrum analyse (const std::vector<double>& cycle)
{
    const int M = (int) cycle.size();
    std::vector<cd> x ((size_t) M);
    for (int i = 0; i < M; ++i) x[(size_t) i] = cycle[(size_t) i];
    srcdsp::fft (x.data(), M, false);
    Spectrum s;
    for (int h = 1; h <= kHarm && h < M / 2; ++h)
    {
        const cd c = x[(size_t) h] * (2.0 / (double) M);
        s.amp[(size_t) h] = std::abs (c);
        s.phase[(size_t) h] = std::arg (c) + kPi * 0.5;   // cos -> sin phase
    }
    return s;
}

/** Writes all mip levels of one frame from its spectrum, normalized so level 0 peaks at 1. */
void synthFrame (const Spectrum& s, float* dst, int levels)
{
    std::vector<cd> x ((size_t) N);
    std::vector<std::vector<double>> out ((size_t) levels, std::vector<double> ((size_t) N));
    for (int L = 0; L < levels; ++L)
    {
        const int maxH = std::max (1, kHarm >> L);
        std::fill (x.begin(), x.end(), cd (0.0, 0.0));
        for (int h = 1; h <= maxH && h < N / 2; ++h)
        {
            const cd c = std::polar (s.amp[(size_t) h] * 0.5, s.phase[(size_t) h] - kPi * 0.5);
            x[(size_t) h] = c;
            x[(size_t) (N - h)] = std::conj (c);
        }
        srcdsp::fft (x.data(), N, true);
        for (int i = 0; i < N; ++i) out[(size_t) L][(size_t) i] = x[(size_t) i].real();
    }
    double peak = 1.0e-9;
    for (double v : out[0]) peak = std::max (peak, std::abs (v));
    const double g = 1.0 / peak;
    for (int L = 0; L < levels; ++L)
        for (int i = 0; i < N; ++i) dst[(size_t) L * N + (size_t) i] = (float) (out[(size_t) L][(size_t) i] * g);
}

std::shared_ptr<AudioResource> newTable (const std::string& name, int frames)
{
    auto r = std::make_shared<AudioResource>();
    r->name = name;
    r->isWavetable = true;
    r->waveFrames = frames;
    r->waveLevels = AudioResource::kWaveMipLevels;
    r->channels = 1;
    r->sampleRate = 48000.0f;
    r->data.assign ((size_t) frames * (size_t) r->waveLevels * (size_t) N, 0.0f);
    return r;
}

std::shared_ptr<const AudioResource> buildTable (const std::string& name, const std::function<Spectrum (int frame, double t)>& gen)
{
    auto r = newTable (name, kTableFrames);
    for (int f = 0; f < kTableFrames; ++f)
    {
        const double t = (double) f / (double) (kTableFrames - 1);
        synthFrame (gen (f, t), r->data.data() + (size_t) f * (size_t) r->waveLevels * (size_t) N, r->waveLevels);
    }
    return r;
}

double smooth (double e0, double e1, double x)
{
    const double t = std::clamp ((x - e0) / (e1 - e0), 0.0, 1.0);
    return t * t * (3.0 - 2.0 * t);
}

// ── the six tables ────────────────────────────────────────────────────────────────────────────

Spectrum glassBell (int, double t)
{
    Spectrum s;
    const double a = smooth (0.0, 0.42, t);                 // 0 at pos 0, 1 at pos 0.42
    s.amp[1] = 1.0 - 0.32 * a;
    s.amp[3] = 0.24 * a; s.phase[3] = 1.0;
    s.amp[5] = 0.10 * a;
    const double u = std::max (0.0, (t - 0.42) / 0.58);
    s.amp[2] = 0.04 * u;
    s.amp[7] = 0.07 * u;
    s.amp[11] = 0.12 * u; s.phase[11] = 0.6;
    s.amp[13] = 0.10 * u; s.phase[13] = 2.1;
    s.amp[17] = 0.08 * u * u; s.phase[17] = 0.3;
    s.amp[19] = 0.05 * u * u;
    s.amp[23] = 0.05 * u * u * u; s.phase[23] = 1.7;
    s.amp[29] = 0.035 * u * u * u;
    return s;
}

Spectrum vowels (int, double t)
{
    static const double F[5][3] = { { 730, 1090, 2440 }, { 530, 1840, 2480 }, { 270, 2290, 3010 }, { 570, 840, 2410 }, { 300, 870, 2240 } };
    static const double G[3] = { 1.0, 0.5, 0.25 };
    static const double B[3] = { 90, 110, 160 };
    const double x = t * 4.0;
    const int i0 = std::min (3, (int) x);
    const double fr = x - i0;
    Spectrum s;
    const double f0 = 110.0;
    for (int h = 1; h <= 160; ++h)
    {
        const double hz = f0 * h;
        double a = 0.0;
        for (int k = 0; k < 3; ++k)
        {
            const double fk = F[i0][k] + (F[i0 + 1][k] - F[i0][k]) * fr;
            const double d = (hz - fk) / B[k];
            a += G[k] / (1.0 + d * d);
        }
        s.amp[(size_t) h] = (a + 0.015) / std::pow ((double) h, 0.35);
        s.phase[(size_t) h] = (h % 2) ? 0.0 : kPi;
    }
    return s;
}

Spectrum harmonic (int, double t)
{
    Spectrum s;
    s.amp[1] = 1.0;
    for (int h = 2; h <= kHarm; ++h)
    {
        double a = 1.0 / h;
        if (t < 0.5) a *= smooth (0.0, 0.5, t) * std::exp (-(double) h * (1.0 - 2.0 * t) * 0.15);
        else if (h % 2 == 0) a *= 1.0 - smooth (0.5, 1.0, t);
        s.amp[(size_t) h] = a;
        s.phase[(size_t) h] = (h % 2) ? 0.0 : kPi;   // ramp-like alignment
    }
    return s;
}

Spectrum digital (int, double t)
{
    constexpr int M = 8192;
    std::vector<double> c ((size_t) M);
    if (t < 0.55)
    {
        const double ratio = 1.0 + (t / 0.55) * 6.0;      // hard-sync sweep
        for (int i = 0; i < M; ++i)
        {
            const double p = (double) i / M;
            const double q = std::fmod (p * ratio, 1.0);
            c[(size_t) i] = (2.0 * q - 1.0) * (1.0 - 0.15 * p);
        }
    }
    else
    {
        const double pw = 0.5 - 0.45 * (t - 0.55) / 0.45;  // PWM
        for (int i = 0; i < M; ++i)
        {
            const double p = (double) i / M;
            c[(size_t) i] = (p < pw ? 1.0 : -1.0) + 0.3 * std::sin (2.0 * kPi * 4.0 * p) * (p < pw ? 1.0 : 0.0);
        }
    }
    return analyse (c);
}

Spectrum metal (int, double t)
{
    constexpr int M = 8192;
    std::vector<double> c ((size_t) M);
    const double I = t * 7.0;
    for (int i = 0; i < M; ++i)
    {
        const double p = 2.0 * kPi * (double) i / M;
        c[(size_t) i] = std::sin (p + I * std::sin (3.0 * p) + 0.45 * I * std::sin (7.0 * p + 0.4));
    }
    return analyse (c);
}

Spectrum breath (int f, double t)
{
    Spectrum s;
    dsp::Rng r (0xB7EA7Bu);
    const double roll = 2.5 + t * 14.0;
    for (int h = 1; h <= 96; ++h)
    {
        const double base = r.uniform() * 2.0 * kPi;
        const double drift = (r.uniform() - 0.5) * 0.9;
        const double jitter = 0.75 + 0.5 * r.uniform();
        s.amp[(size_t) h] = std::exp (-(double) (h - 1) / roll) * (h == 1 ? 1.0 : 0.55 * jitter);
        s.phase[(size_t) h] = base + drift * f;
    }
    return s;
}

// ── procedural samples (48 kHz) ──────────────────────────────────────────────────────────────

constexpr float kSR = 48000.0f;

void normalize (std::vector<float>& d, float peak)
{
    float m = 1.0e-9f;
    for (float v : d) m = std::max (m, std::abs (v));
    for (float& v : d) v *= peak / m;
}

/** Decaying sine partial added into a mono buffer (phase accumulator with the shared sine table). */
void addPartial (std::vector<float>& d, int channels, int ch, int start, float hz, float amp, float decaySec, float beatHz = 0.0f, float attack = 0.0005f)
{
    const int frames = (int) d.size() / channels;
    const float inc = hz / kSR;
    const float k = std::exp (-1.0f / (decaySec * kSR));
    const float aStep = 1.0f / std::max (1.0f, attack * kSR);
    float ph = 0.0f, env = amp, att = 0.0f, bp = 0.0f;
    const float binc = beatHz / kSR;
    for (int i = start; i < frames; ++i)
    {
        att = std::min (1.0f, att + aStep);
        float v = srcdsp::sin2pi (ph) * env * att;
        if (beatHz > 0.0f) { v *= 0.6f + 0.4f * srcdsp::cos2pi (bp); bp += binc; }
        d[(size_t) i * channels + ch] += v;
        ph += inc; if (ph >= 1.0f) ph -= 1.0f;
        env *= k;
        if (env < 1.0e-5f) break;
    }
}

std::shared_ptr<AudioResource> newSample (const std::string& name, int channels, float seconds)
{
    auto r = std::make_shared<AudioResource>();
    r->name = name; r->channels = channels; r->sampleRate = kSR;
    r->data.assign ((size_t) (seconds * kSR) * (size_t) channels, 0.0f);
    return r;
}

std::shared_ptr<const AudioResource> rainTin()
{
    auto r = newSample ("rain_tin.wav", 2, 6.0f);
    auto& d = r->data;
    dsp::Rng rng (0x5A1Au);
    const int frames = r->frames();
    for (int n = 0; n < 260; ++n)
    {
        const int at = (int) (rng.uniform() * (float) (frames - 4000));
        const float base = 1800.0f + 4200.0f * rng.uniform();
        const float amp = 0.15f + 0.85f * rng.uniform() * rng.uniform();
        const float pan = rng.uniform();
        const float ratios[3] = { 1.0f, 1.47f + 0.2f * rng.uniform(), 2.31f + 0.3f * rng.uniform() };
        for (int k = 0; k < 3; ++k)
        {
            const float dec = 0.012f + 0.05f * rng.uniform();
            addPartial (d, 2, 0, at, base * ratios[k], amp * (1.0f - pan) / (float) (k + 1), dec);
            addPartial (d, 2, 1, at, base * ratios[k], amp * pan / (float) (k + 1), dec);
        }
    }
    // soft noise bed
    float lpL = 0.0f, lpR = 0.0f;
    for (int i = 0; i < frames; ++i)
    {
        lpL += (rng.bipolar() - lpL) * 0.08f;
        lpR += (rng.bipolar() - lpR) * 0.08f;
        d[(size_t) i * 2] += lpL * 0.06f;
        d[(size_t) i * 2 + 1] += lpR * 0.06f;
    }
    normalize (d, 0.8f);
    return r;
}

std::shared_ptr<const AudioResource> glassTap()
{
    auto r = newSample ("glass_tap.wav", 1, 5.0f);
    dsp::Rng rng (0x61A55u);
    static const float scale[] = { 1046.5f, 1174.7f, 1318.5f, 1568.0f, 1760.0f, 2093.0f };
    for (int n = 0; n < 12; ++n)
    {
        const int at = (int) ((0.05f + 0.4f * (float) n + 0.08f * rng.uniform()) * kSR);
        const float f = scale[rng.next() % 6];
        const float amp = 0.5f + 0.5f * rng.uniform();
        const float ratios[4] = { 1.0f, 2.32f, 4.25f, 6.63f };
        const float decs[4] = { 1.4f, 0.8f, 0.4f, 0.2f };
        for (int k = 0; k < 4; ++k) addPartial (r->data, 1, 0, at, f * ratios[k], amp / (float) (1 + k * 1.5f), decs[k] * (0.7f + 0.6f * rng.uniform()));
    }
    normalize (r->data, 0.8f);
    return r;
}

std::shared_ptr<const AudioResource> choirAir()
{
    auto r = newSample ("choir_air.wav", 2, 6.0f);
    auto& d = r->data;
    const int frames = r->frames();
    static const float notes[] = { 57.0f, 60.0f, 64.0f, 69.0f };
    static const double F[3] = { 570, 840, 2410 }, G[3] = { 1.0, 0.45, 0.2 }, B[3] = { 80, 100, 150 };
    dsp::Rng rng (0xC401u);
    for (int v = 0; v < 4; ++v)
    {
        const float f0 = dsp::noteToHz (notes[v]);
        const float vibRate = 4.6f + 0.7f * rng.uniform(), vibPh0 = rng.uniform();
        const float pan = 0.2f + 0.6f * (float) v / 3.0f;
        for (int h = 1; h <= 36; ++h)
        {
            const double hz = f0 * h;
            if (hz > 9000.0) break;
            double a = 0.0;
            for (int k = 0; k < 3; ++k) { const double x = (hz - F[k]) / B[k]; a += G[k] / (1.0 + x * x); }
            a = (a + 0.01) / std::sqrt ((double) h) * 0.25;
            float ph = rng.uniform();
            float vp = vibPh0;
            for (int i = 0; i < frames; ++i)
            {
                const float t = (float) i / kSR;
                const float env = std::min (1.0f, t / 0.8f) * std::min (1.0f, (6.0f - t) / 0.8f);
                const float vib = 1.0f + 0.004f * srcdsp::sin2pi (vp);
                vp += vibRate / kSR;
                ph += (float) hz * vib / kSR;
                ph -= std::floor (ph);
                const float s = srcdsp::sin2pi (ph) * (float) a * env;
                d[(size_t) i * 2] += s * (1.0f - pan);
                d[(size_t) i * 2 + 1] += s * pan;
            }
        }
    }
    // breath
    float lp = 0.0f, bp = 0.0f;
    for (int i = 0; i < frames; ++i)
    {
        const float t = (float) i / kSR;
        const float env = std::min (1.0f, t / 1.0f) * std::min (1.0f, (6.0f - t) / 1.0f);
        const float w = rng.bipolar();
        lp += (w - lp) * 0.25f; bp += (lp - bp) * 0.05f;
        const float br = (lp - bp) * 0.05f * env;
        d[(size_t) i * 2] += br; d[(size_t) i * 2 + 1] += br * 0.9f;
    }
    normalize (d, 0.75f);
    return r;
}

std::shared_ptr<const AudioResource> metalBowl()
{
    auto r = newSample ("metal_bowl.wav", 1, 7.0f);
    const float f0 = 220.0f;
    const float ratios[5] = { 1.0f, 2.71f, 5.15f, 8.1f, 11.6f };
    const float amps[5] = { 1.0f, 0.6f, 0.35f, 0.2f, 0.1f };
    const float beats[5] = { 0.7f, 1.9f, 2.6f, 3.3f, 4.1f };
    for (int k = 0; k < 5; ++k)
        addPartial (r->data, 1, 0, 0, f0 * ratios[k], amps[k], 5.5f / (1.0f + 0.6f * k), beats[k], 0.004f);
    normalize (r->data, 0.8f);
    return r;
}

std::shared_ptr<const AudioResource> vinylDust()
{
    auto r = newSample ("vinyl_dust.wav", 2, 5.0f);
    auto& d = r->data;
    dsp::Rng rng (0xD057u);
    const int frames = r->frames();
    float hissL = 0.0f, hissR = 0.0f, rumble = 0.0f;
    float crack = 0.0f;
    for (int i = 0; i < frames; ++i)
    {
        hissL += (rng.bipolar() - hissL) * 0.5f;
        hissR += (rng.bipolar() - hissR) * 0.5f;
        rumble += (rng.bipolar() * 0.5f - rumble) * 0.002f;
        if (rng.uniform() < 0.0012f) crack += rng.bipolar() * (0.3f + 0.7f * rng.uniform());
        if (rng.uniform() < 0.00004f) crack += rng.bipolar() * 2.0f;   // the odd big pop
        const float c = crack;
        crack *= 0.82f;
        const float wow = 0.03f * srcdsp::sin2pi ((float) i / kSR * 0.55f);
        d[(size_t) i * 2] = hissL * 0.03f + rumble * 0.4f + c + wow * 0.0f;
        d[(size_t) i * 2 + 1] = hissR * 0.03f + rumble * 0.4f + c * 0.9f;
    }
    normalize (d, 0.7f);
    return r;
}

std::shared_ptr<const AudioResource> kalimba()
{
    auto r = newSample ("kalimba.wav", 1, 6.0f);
    static const float pent[] = { 60, 62, 64, 67, 69, 72, 74, 76, 79, 81 };
    dsp::Rng rng (0x4A11u);
    for (int n = 0; n < 16; ++n)
    {
        const int at = (int) ((0.02f + 0.35f * (float) n) * kSR);
        const float f = dsp::noteToHz (pent[rng.next() % 10]);
        const float amp = 0.6f + 0.4f * rng.uniform();
        addPartial (r->data, 1, 0, at, f, amp, 1.2f, 0.0f, 0.001f);
        addPartial (r->data, 1, 0, at, f * 5.4f, amp * 0.35f, 0.08f, 0.0f, 0.0005f);
        addPartial (r->data, 1, 0, at, f * 9.1f, amp * 0.15f, 0.04f, 0.0f, 0.0005f);
        addPartial (r->data, 1, 0, at, f * 2.0f, amp * 0.12f, 0.5f, 0.0f, 0.001f);
    }
    normalize (r->data, 0.8f);
    return r;
}

/** Autocorrelation pitch period (frames) of a mono signal, or 0 when it is not clearly pitched. */
int detectPeriod (const std::vector<float>& x, float sampleRate)
{
    const int n = (int) x.size();
    const int minP = std::max (2, (int) (sampleRate / 2000.0f));
    const int maxP = std::min (n / 3, (int) (sampleRate / 30.0f));
    if (maxP <= minP + 2) return 0;
    // analyse a window from the loudest region
    const int win = std::min (n - maxP, (int) (sampleRate * 0.25f));
    if (win < maxP) return 0;
    int start = 0;
    {
        double best = -1.0;
        for (int s = 0; s + win + maxP < n; s += win / 2)
        {
            double e = 0.0;
            for (int i = 0; i < win; i += 4) e += (double) x[(size_t) (s + i)] * x[(size_t) (s + i)];
            if (e > best) { best = e; start = s; }
        }
    }
    double e0 = 0.0;
    for (int i = 0; i < win; ++i) e0 += (double) x[(size_t) (start + i)] * x[(size_t) (start + i)];
    if (e0 <= 1.0e-9) return 0;
    double bestR = 0.0;
    int bestP = 0;
    std::vector<double> rv ((size_t) maxP + 1, 0.0);
    for (int p = minP; p <= maxP; ++p)
    {
        double acc = 0.0, e1 = 0.0;
        for (int i = 0; i < win; ++i)
        {
            const double a = x[(size_t) (start + i)], b = x[(size_t) (start + i + p)];
            acc += a * b; e1 += b * b;
        }
        rv[(size_t) p] = acc / std::sqrt (e0 * e1 + 1.0e-12);
    }
    // first peak above 0.85 of the global best (avoids octave errors)
    for (int p = minP; p <= maxP; ++p) bestR = std::max (bestR, rv[(size_t) p]);
    if (bestR < 0.8) return 0;
    for (int p = minP + 1; p < maxP; ++p)
        if (rv[(size_t) p] >= 0.9 * bestR && rv[(size_t) p] >= rv[(size_t) (p - 1)] && rv[(size_t) p] >= rv[(size_t) (p + 1)]) { bestP = p; break; }
    return bestP;
}

} // namespace

const std::vector<std::string>& builtinSampleNames()
{
    static const std::vector<std::string> n { "rain_tin.wav", "glass_tap.wav", "choir_air.wav", "metal_bowl.wav", "vinyl_dust.wav", "kalimba.wav" };
    return n;
}

std::shared_ptr<const AudioResource> builtinSample (const std::string& name)
{
    static std::map<std::string, std::shared_ptr<const AudioResource>> cache;
    std::lock_guard<std::mutex> g (cacheMutex());
    if (auto it = cache.find (name); it != cache.end()) return it->second;
    std::shared_ptr<const AudioResource> r;
    if (name == "rain_tin.wav") r = rainTin();
    else if (name == "glass_tap.wav") r = glassTap();
    else if (name == "choir_air.wav") r = choirAir();
    else if (name == "metal_bowl.wav") r = metalBowl();
    else if (name == "vinyl_dust.wav") r = vinylDust();
    else if (name == "kalimba.wav") r = kalimba();
    if (r) cache[name] = r;
    return r;
}

std::shared_ptr<const AudioResource> builtinWavetable (int index)
{
    static std::shared_ptr<const AudioResource> cache[6];
    if (index < 0 || index > 5) return nullptr;
    std::lock_guard<std::mutex> g (cacheMutex());
    if (cache[index]) return cache[index];
    static const char* names[6] = { "Glass Bell", "Vowels", "Harmonic", "Digital", "Metal", "Breath" };
    using Gen = Spectrum (*) (int, double);
    static const Gen gens[6] = { glassBell, vowels, harmonic, digital, metal, breath };
    cache[index] = buildTable (names[index], gens[index]);
    return cache[index];
}

std::shared_ptr<const AudioResource> makeWavetableFromSample (const AudioResource& sample, const std::string& name)
{
    const int frames = sample.frames();
    if (frames < 64 || sample.channels < 1) return nullptr;
    std::vector<float> mono ((size_t) frames);
    for (int i = 0; i < frames; ++i)
    {
        float s = 0.0f;
        for (int c = 0; c < sample.channels; ++c) s += sample.data[(size_t) i * (size_t) sample.channels + (size_t) c];
        mono[(size_t) i] = s / (float) sample.channels;
    }
    const int period = detectPeriod (mono, sample.sampleRate);
    int cycleLen, count;
    std::vector<int> starts;
    if (period > 0)
    {
        cycleLen = period;
        const int available = frames / period;
        count = std::clamp (available, 1, 64);
        for (int k = 0; k < count; ++k)
        {
            const int cycleIndex = count > 1 ? (int) ((double) k * (available - 1) / (count - 1)) : 0;
            starts.push_back (cycleIndex * period);
        }
    }
    else
    {
        count = std::clamp (frames / 256, 1, 64);
        cycleLen = frames / count;
        for (int k = 0; k < count; ++k) starts.push_back (k * cycleLen);
    }

    auto r = newTable (name, count);
    constexpr int M = N;
    for (int k = 0; k < count; ++k)
    {
        // resample the cycle to N points (linear), then band-limit per level via the spectrum
        std::vector<double> cyc ((size_t) M);
        for (int i = 0; i < M; ++i)
        {
            const double pos = (double) starts[(size_t) k] + (double) i * cycleLen / M;
            const int i0 = std::min (frames - 1, (int) pos);
            const int i1 = std::min (frames - 1, i0 + 1);
            const double f = pos - i0;
            cyc[(size_t) i] = mono[(size_t) i0] + (mono[(size_t) i1] - mono[(size_t) i0]) * f;
        }
        // remove the cycle's linear trend so its ends meet (no click at the wrap)
        const double slope = (cyc[(size_t) (M - 1)] - cyc[0]) / M;
        for (int i = 0; i < M; ++i) cyc[(size_t) i] -= slope * i;
        Spectrum s = analyse (cyc);
        double energy = 0.0;
        for (double a : s.amp) energy += a * a;
        if (energy < 1.0e-12) s.amp[1] = 1.0;
        synthFrame (s, r->data.data() + (size_t) k * (size_t) r->waveLevels * (size_t) N, r->waveLevels);
    }
    return r;
}

std::shared_ptr<const AudioResource> makeSampleResource (const std::string& name, float sr, int ch, std::vector<float> data)
{
    auto r = std::make_shared<AudioResource>();
    r->name = name;
    r->sampleRate = sr > 0.0f ? sr : 48000.0f;
    ch = std::max (1, ch);
    const size_t frames = data.size() / (size_t) ch;
    const size_t cap = std::min (frames, (size_t) (r->sampleRate * 30.0f));
    const int outCh = std::min (2, ch);
    r->channels = outCh;
    if (ch == outCh && cap == frames) { r->data = std::move (data); return r; }
    r->data.assign (cap * (size_t) outCh, 0.0f);
    for (size_t i = 0; i < cap; ++i)
    {
        if (ch <= 2)
            for (int c = 0; c < outCh; ++c) r->data[i * (size_t) outCh + (size_t) c] = data[i * (size_t) ch + (size_t) c];
        else
        {
            float l = 0.0f, rr = 0.0f; int nl = 0, nr = 0;
            for (int c = 0; c < ch; ++c)
            {
                const float v = data[i * (size_t) ch + (size_t) c];
                if (c % 2 == 0) { l += v; ++nl; } else { rr += v; ++nr; }
            }
            r->data[i * 2] = l / (float) std::max (1, nl);
            r->data[i * 2 + 1] = rr / (float) std::max (1, nr);
        }
    }
    return r;
}

} // namespace weft
