#pragma once
#include <juce_audio_utils/juce_audio_utils.h>
#include <juce_dsp/juce_dsp.h>
#include <cmath>

struct Mkvst04Sound : public juce::SynthesiserSound
{
    bool appliesToNote (int) override    { return true; }
    bool appliesToChannel (int) override { return true; }
};

// Eine Stimme von mkvst0_4. Klangkette:
//   2 Oszillatoren + Sub + FM-Metall -> Tiefpass (Dunkel) -> Uhr-Gate -> + Klack -> Knatter -> Hüllkurve
//   Ein LFO (pro Stimme, startet mit der Note) moduliert Filter, Tonhöhe und Lautstärke.
//
// Mathematische Grundlagen (alles Standard-DSP, fs = Abtastrate, f = Tonhöhe):
//  * Phasenakkumulator: phi[n+1] = frac(phi[n] + f/fs), phi in [0,1).
//  * Wellenformen aus der Phase: Sinus sin(2*pi*phi), Sägezahn 2*phi-1, Rechteck sgn(0,5-phi),
//    Dreieck 1-4*|phi-0,5|. Säge und Rechteck sind "naiv" (erzeugen Aliasing).
//  * Oktave und Verstimmung: f' = f * 2^(Oktave + Cent/1200). Eine Oktave verdoppelt die Frequenz,
//    100 Cent sind ein Halbton (Faktor 2^(1/12)).
//  * Mix: y = 0,75 * ((1-m)*osc1 + m*osc2) + sub*0,5*sin(2*pi*phi_sub), phi_sub läuft bei f/2.
//  * FM: y = sin(2*pi*phi_c + I*sin(2*pi*phi_m)), f_m/f_c = sqrt(2) ist irrational, die Seitenbänder
//    f_c +- k*f_m liegen nicht auf einem harmonischen Raster (metallisch). Stärke folgt J_k(I).
//  * LFO: gleiche Wellenformen bei 0,05 bis 20 Hz; "S&H" zieht bei jedem Phasenüberlauf einen neuen
//    Zufallswert in [-1,1] und hält ihn. Die Modulation wirkt exponentiell, weil wir Tonhöhen und
//    Frequenzen logarithmisch hören:
//        Filter:  fc' = fc * 2^(2 * Tiefe * lfo)        (bis +-2 Oktaven)
//        Tonhöhe: f'  = f  * 2^(2 * Tiefe * lfo / 12)   (bis +-2 Halbtöne, Vibrato)
//        Lautst.: g   = 1 - Tiefe * (0,5 - 0,5*lfo)     (Tremolo)
//  * Tiefpass: topologieerhaltender Zustandsvariablenfilter (Zavalishin), g = tan(pi*fc/fs).
//  * Uhr: Gate = (1-d) + d*(1-phi_clock)^3. Klack: weißes Rauschen -> Bandpass -> exp. Hüllkurve
//    e^(-t/tau), tau = 18 ms (pro Abtastwert Faktor exp(-1/(fs*tau))).
//  * Knatter (Bitcrusher): x' = round(x * 2^b) / 2^b, b = 14 - 11*k, plus Sample-and-Hold.
//  * Sättigung am Ende: y = tanh(x), begrenzt weich auf [-1, 1].
class Mkvst04Voice : public juce::SynthesiserVoice
{
public:
    explicit Mkvst04Voice (juce::AudioProcessorValueTreeState& state) : apvts (state) {}

    void prepare (double sampleRate, int blockSize)
    {
        sr = sampleRate;
        juce::dsp::ProcessSpec spec { sampleRate, (juce::uint32) blockSize, 1 };

        lowpass.prepare (spec);
        lowpass.setType (juce::dsp::StateVariableTPTFilterType::lowpass);
        lowpass.setResonance (1.1f);

        clickFilter.prepare (spec);
        clickFilter.setType (juce::dsp::StateVariableTPTFilterType::bandpass);
        clickFilter.setResonance (6.0f);

        adsr.setSampleRate (sampleRate);
    }

    bool canPlaySound (juce::SynthesiserSound* s) override
    {
        return dynamic_cast<Mkvst04Sound*> (s) != nullptr;
    }

    void startNote (int midiNote, float velocity, juce::SynthesiserSound*, int) override
    {
        freq = (float) juce::MidiMessage::getMidiNoteInHertz (midiNote);
        level = velocity;
        phOsc1 = phOsc2 = phSub = phCar = phMod = 0.0f;
        clockPhase = 0.0f;
        clickEnv = 1.0f;                       // "Klack" beim Anschlagen
        lfoPhase = 0.0f;                       // LFO startet mit der Note
        shValue = rng.nextFloat() * 2.0f - 1.0f;
        holdCounter = 0;
        held = 0.0f;
        lowpass.reset();
        clickFilter.reset();
        updateAdsr();
        adsr.noteOn();
    }

    void stopNote (float, bool allowTailOff) override
    {
        if (allowTailOff)
        {
            updateAdsr();
            adsr.noteOff();
        }
        else
        {
            adsr.reset();
            clearCurrentNote();
        }
    }

    void pitchWheelMoved (int) override {}
    void controllerMoved (int, int) override {}

    void renderNextBlock (juce::AudioBuffer<float>& out, int startSample, int numSamples) override
    {
        if (! isVoiceActive())
            return;

        const float dark       = param ("dark");
        const float klack      = param ("klack");
        const float knatter    = param ("knatter");
        const float mystery    = param ("mystery");
        const float clockRate  = param ("clockRate");
        const float clockDepth = param ("clockDepth");

        const int   wave1   = (int) std::round (param ("osc1Wave"));
        const int   wave2   = (int) std::round (param ("osc2Wave"));
        const float mul1    = std::exp2 (std::round (param ("osc1Oct")));
        const float mul2    = std::exp2 (std::round (param ("osc2Oct")) + param ("osc2Detune") / 1200.0f);
        const float mix     = param ("oscMix");
        const float subAmt  = param ("sub");

        const int   lfoShape  = (int) std::round (param ("lfoShape"));
        const float lfoRate   = param ("lfoRate");
        const float lfoFilter = param ("lfoFilter");
        const float lfoPitch  = param ("lfoPitch");
        const float lfoAmp    = param ("lfoAmp");

        const float fs      = (float) sr;
        const float twoPi   = juce::MathConstants<float>::twoPi;
        const float baseCut = 200.0f + std::pow (1.0f - dark, 2.0f) * 9000.0f;

        clickFilter.setCutoffFrequency (juce::jlimit (300.0f, fs * 0.45f, freq * 5.0f + 1500.0f));
        const float clickDecay = std::exp (-1.0f / (fs * 0.018f));          // ca. 18 ms

        const int   hold    = 1 + (int) (knatter * 14.0f);                  // Sample-Hold
        const float step    = std::pow (2.0f, 14.0f - knatter * 11.0f);     // Bit-Tiefe 14 -> 3
        const float fmIndex = mystery * 4.0f;

        for (int i = 0; i < numSamples; ++i)
        {
            // LFO
            lfoPhase += lfoRate / fs;
            if (lfoPhase >= 1.0f)
            {
                lfoPhase -= 1.0f;
                if (lfoShape == 4) shValue = rng.nextFloat() * 2.0f - 1.0f;
            }
            const float lfo = lfoValue (lfoShape);

            if ((i & 7) == 0)
            {
                const float cut = baseCut * std::exp2 (2.0f * lfoFilter * lfo);
                lowpass.setCutoffFrequency (juce::jlimit (40.0f, fs * 0.45f, cut));
            }
            const float pitchMul = std::exp2 (2.0f * lfoPitch * lfo / 12.0f);
            const float ampMod   = 1.0f - lfoAmp * (0.5f - 0.5f * lfo);

            // Oszillatoren
            const float w1      = wave (wave1, phOsc1);
            const float w2      = wave (wave2, phOsc2);
            const float sub     = std::sin (twoPi * phSub);
            const float carrier = std::sin (twoPi * phCar + fmIndex * std::sin (twoPi * phMod));
            const float osc     = 0.75f * ((1.0f - mix) * w1 + mix * w2) + subAmt * 0.5f * sub + mystery * 0.5f * carrier;

            const float f = freq * pitchMul;
            phOsc1 = wrap (phOsc1 + f * mul1 / fs);
            phOsc2 = wrap (phOsc2 + f * mul2 / fs);
            phSub  = wrap (phSub  + f * 0.5f / fs);
            phCar  = wrap (phCar  + f * 2.0f / fs);
            phMod  = wrap (phMod  + f * 2.0f * 1.4142f / fs);

            // Uhr: tickt, bei jedem Tick gibt es ein kleines "Klack"
            clockPhase += clockRate / fs;
            if (clockPhase >= 1.0f)
            {
                clockPhase -= 1.0f;
                clickEnv = juce::jmax (clickEnv, 0.6f * clockDepth);
            }
            const float tick = std::pow (1.0f - clockPhase, 3.0f);
            const float gate = (1.0f - clockDepth) + clockDepth * tick;

            const float body  = lowpass.processSample (0, osc) * gate;
            const float noise = rng.nextFloat() * 2.0f - 1.0f;
            const float click = clickFilter.processSample (0, noise) * clickEnv;
            clickEnv *= clickDecay;

            const float dry = body + click * klack * 1.5f;

            // Knatter: Bitcrusher
            if (holdCounter <= 0)
            {
                held = std::floor (dry * step + 0.5f) / step;
                holdCounter = hold;
            }
            --holdCounter;
            const float mixed = dry + knatter * (held - dry);

            const float s = std::tanh (mixed * 1.2f) * adsr.getNextSample() * level * 0.35f * ampMod;

            for (int ch = 0; ch < out.getNumChannels(); ++ch)
                out.addSample (ch, startSample + i, s);
        }

        if (! adsr.isActive())
            clearCurrentNote();
    }

private:
    static float wrap (float p) { return p - std::floor (p); }
    float param (const char* id) const { return apvts.getRawParameterValue (id)->load(); }

    // 0 Sinus, 1 Säge, 2 Rechteck, 3 Dreieck
    static float wave (int type, float p)
    {
        switch (type)
        {
            case 0:  return std::sin (juce::MathConstants<float>::twoPi * p);
            case 1:  return 2.0f * p - 1.0f;
            case 2:  return p < 0.5f ? 1.0f : -1.0f;
            default: return 1.0f - 4.0f * std::abs (p - 0.5f);
        }
    }

    // LFO-Formen: 0 Sinus, 1 Dreieck, 2 Rechteck, 3 Säge, 4 Sample & Hold
    float lfoValue (int shape) const
    {
        switch (shape)
        {
            case 0:  return std::sin (juce::MathConstants<float>::twoPi * lfoPhase);
            case 1:  return 1.0f - 4.0f * std::abs (lfoPhase - 0.5f);
            case 2:  return lfoPhase < 0.5f ? 1.0f : -1.0f;
            case 3:  return 2.0f * lfoPhase - 1.0f;
            default: return shValue;
        }
    }

    void updateAdsr()
    {
        adsr.setParameters ({ param ("attack"), 0.5f, 0.7f, param ("release") });
    }

    juce::AudioProcessorValueTreeState& apvts;
    juce::dsp::StateVariableTPTFilter<float> lowpass, clickFilter;
    juce::ADSR adsr;
    juce::Random rng;

    double sr = 44100.0;
    float freq = 440.0f, level = 1.0f;
    float phOsc1 = 0.0f, phOsc2 = 0.0f, phSub = 0.0f, phCar = 0.0f, phMod = 0.0f;
    float clockPhase = 0.0f, clickEnv = 0.0f, lfoPhase = 0.0f, shValue = 0.0f, held = 0.0f;
    int holdCounter = 0;
};
