#pragma once
#include <juce_audio_utils/juce_audio_utils.h>
#include <algorithm>
#include <array>
#include <cmath>
#include <vector>

// Zwei eigene Effekte (keine fertigen Standard-Hall- oder Delay-Module):
//
//  DarkEcho   - Ping-Pong-Echo, dessen Wiederholungen mit jedem Durchlauf dunkler und
//               leicht sättigender werden und deren Zeit unregelmäßig "atmet" (Drift).
//  VoidReverb - Feedback-Delay-Network (FDN) mit 8 Verzögerungsleitungen, Hadamard-Mischmatrix,
//               Dämpfung je Leitung, langsam modulierten Leitungslängen und Sättigung im
//               Rückweg ("Spuk"), dadurch klingt der Hall geisterhaft und leicht schwebend.

// Verzögerungsleitung mit linearer Interpolation für Bruchteile von Abtastwerten.
class FxDelayLine
{
public:
    void prepare (int maxSamples)
    {
        buf.assign ((size_t) juce::jmax (8, maxSamples) + 4, 0.0f);
        w = 0;
    }

    void clear() { std::fill (buf.begin(), buf.end(), 0.0f); }

    void push (float x)
    {
        buf[(size_t) w] = x;
        if (++w >= (int) buf.size()) w = 0;
    }

    // delay = Abstand zum neuesten Wert in Abtastwerten (mindestens 1)
    float read (float delay) const
    {
        const int n = (int) buf.size();
        delay = juce::jlimit (1.0f, (float) (n - 3), delay);
        float r = (float) w - 1.0f - delay;
        if (r < 0.0f) r += (float) n;
        const int i0 = juce::jlimit (0, n - 1, (int) r);
        const float f = r - (float) i0;
        const int i1 = (i0 + 1) % n;
        return buf[(size_t) i0] * (1.0f - f) + buf[(size_t) i1] * f;
    }

private:
    std::vector<float> buf;
    int w = 0;
};

//==============================================================================
// Mathematik des Echos:
//   Verzögerung   d = T * fs * (1 + 0,03 * Drift * wob(t)), wob = 0,6 sin(w t) + 0,4 sin(2,7 w t + 1,3)
//   Rückweg       y[n] = x[n] + g * sat(lp(tap_gegenüber)), kreuzweise = Ping-Pong
//   Tiefpass      lp: y += (1 - c) * (x - y), c = exp(-2 pi fc / fs), fc = 700 + (1 - dunkel)^2 * 11000 Hz
//   Sättigung     sat(x) = tanh(s x) / s  (weich begrenzend, lässt kleine Signale unverändert)
class DarkEcho
{
public:
    void prepare (double sampleRate)
    {
        fs = (float) sampleRate;
        for (auto& l : line) l.prepare ((int) (fs * 1.6f));
        reset();
    }

    void reset()
    {
        for (auto& l : line) l.clear();
        lp[0] = lp[1] = 0.0f;
        phase = 0.0f;
        timeSm = timeTarget;
    }

    void setParams (float timeMs, float fb, float dark, float drift, float mixAmount)
    {
        timeTarget = timeMs;
        feedback   = juce::jmin (0.92f, fb);
        driftAmt   = drift;
        mix        = mixAmount;
        const float fc = 700.0f + std::pow (1.0f - dark, 2.0f) * 11000.0f;
        lpCoef   = std::exp (-juce::MathConstants<float>::twoPi * fc / fs);
        satDrive = 1.0f + 1.5f * dark;
    }

    void process (float& l, float& r)
    {
        timeSm += (timeTarget - timeSm) * 0.0008f;                 // weiche Zeitänderung ohne Knacksen
        phase += (0.23f + 0.9f * driftAmt) / fs;
        if (phase >= 1.0f) phase -= 1.0f;
        const float tp  = juce::MathConstants<float>::twoPi * phase;
        const float wob = 0.6f * std::sin (tp) + 0.4f * std::sin (2.7f * tp + 1.3f);
        const float d   = juce::jmax (2.0f, timeSm * 0.001f * fs * (1.0f + 0.03f * driftAmt * wob));

        const float tapL = line[0].read (d);
        const float tapR = line[1].read (d * (1.0f + 0.25f * driftAmt));   // rechts leicht versetzt

        lp[0] += (1.0f - lpCoef) * (tapR - lp[0]);                  // kreuzweise Rückkopplung
        lp[1] += (1.0f - lpCoef) * (tapL - lp[1]);

        const float mono = 0.5f * (l + r);
        line[0].push (mono + feedback * sat (lp[0]));
        line[1].push (feedback * sat (lp[1]));

        l += tapL * mix * 1.2f;
        r += tapR * mix * 1.2f;
    }

private:
    float sat (float x) const { return std::tanh (x * satDrive) / satDrive; }

    std::array<FxDelayLine, 2> line;
    float fs = 44100.0f, lpCoef = 0.5f, satDrive = 1.0f;
    float timeTarget = 380.0f, timeSm = 380.0f, feedback = 0.4f, driftAmt = 0.3f, mix = 0.2f;
    float phase = 0.0f;
    float lp[2] = { 0.0f, 0.0f };
};

//==============================================================================
// Mathematik des Halls (FDN):
//   Je Leitung i: Länge L_i (Primzahl-nahe Werte, damit sich keine Resonanzen überlagern)
//   Verstärkung   g_i = 10^(-3 L_i / (fs * RT60))   (nach RT60 Sekunden ist der Pegel -60 dB)
//   Mischmatrix   H8 = Hadamard / sqrt(8), orthogonal = verlustfrei, verteilt jedes Signal auf alle Leitungen
//   Modulation    L_i' = L_i * (1 + Spuk * 0,006 * sin(2 pi f_i t)), f_i = 0,11 + 0,037 i Hz
//   Rückweg       x_i = g_i * tanh(s v_i) / s, s = 1 + 1,5 * Spuk
class VoidReverb
{
public:
    static constexpr int N = 8;

    void prepare (double sampleRate)
    {
        fs = (float) sampleRate;
        scale = fs / 44100.0f;
        for (auto& l : line) l.prepare ((int) (fs * 0.25f));
        reset();
        setParams (0.5f, 0.5f, 0.3f, 0.3f);
    }

    void reset()
    {
        for (auto& l : line) l.clear();
        for (int i = 0; i < N; ++i) { damp[i] = 0.0f; lfo[i] = 0.137f * (float) i; }
    }

    void setParams (float size, float dampAmount, float spook, float mixAmount)
    {
        static const float base[N] = { 1117.0f, 1327.0f, 1559.0f, 1721.0f, 1949.0f, 2131.0f, 2377.0f, 2593.0f };
        const float sz   = 0.6f + 1.2f * size;
        const float rt60 = 0.5f + 10.0f * std::pow (size, 1.5f);
        for (int i = 0; i < N; ++i)
        {
            len[i]  = base[i] * scale * sz;
            gain[i] = std::pow (10.0f, -3.0f * len[i] / (fs * rt60));
        }
        dampK  = juce::jmap (dampAmount, 0.95f, 0.08f);      // je mehr Dämpfung, desto dunkler der Nachhall
        spookA = spook;
        satS   = 1.0f + 1.5f * spook;
        mix    = mixAmount;
    }

    void process (float& l, float& r)
    {
        const float in = 0.5f * (l + r);
        float v[N];

        for (int i = 0; i < N; ++i)
        {
            lfo[i] += (0.11f + 0.037f * (float) i) / fs;
            if (lfo[i] >= 1.0f) lfo[i] -= 1.0f;
            const float m = 1.0f + spookA * 0.006f * std::sin (juce::MathConstants<float>::twoPi * lfo[i]);
            const float raw = line[(size_t) i].read (len[i] * m);
            damp[i] += dampK * (raw - damp[i]);
            v[i] = damp[i];
        }

        hadamard8 (v);

        for (int i = 0; i < N; ++i)
        {
            const float x = std::tanh (v[i] * satS) / satS * gain[i];
            line[(size_t) i].push (in * 0.3f + x);
        }

        const float outL = 0.35f * (damp[0] - damp[2] + damp[4] - damp[6]);
        const float outR = 0.35f * (damp[1] - damp[3] + damp[5] - damp[7]);
        l += outL * mix * 1.5f;
        r += outR * mix * 1.5f;
    }

private:
    static void hadamard8 (float* v)
    {
        for (int h = 1; h < 8; h *= 2)
            for (int i = 0; i < 8; i += h * 2)
                for (int j = i; j < i + h; ++j)
                {
                    const float a = v[j], b = v[j + h];
                    v[j] = a + b;
                    v[j + h] = a - b;
                }
        for (int i = 0; i < 8; ++i) v[i] *= 0.35355339f;
    }

    std::array<FxDelayLine, N> line;
    float fs = 44100.0f, scale = 1.0f;
    float len[N] {}, gain[N] {}, damp[N] {}, lfo[N] {};
    float dampK = 0.5f, spookA = 0.3f, satS = 1.0f, mix = 0.25f;
};
