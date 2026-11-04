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
//   2 Oszillatoren (Sync, FM/AM, Spread) + Sub + FM-Metall -> Tiefpass (Dunkel) -> Uhr-Gate -> + Klack
//   -> Knatter -> Hüllkurve (ADSFR)
//   Ein LFO (pro Stimme, startet mit der Note) moduliert Filter, Tonhöhe und Lautstärke.
//
// Mathematische Grundlagen (alles Standard-DSP, fs = Abtastrate, f = Tonhöhe):
//  * Phasenakkumulator: phi[n+1] = frac(phi[n] + f/fs), phi in [0,1).
//  * Wellenformen aus der Phase: Sinus sin(2*pi*phi), Sägezahn 2*phi-1, Rechteck sgn(0,5-phi),
//    Dreieck 1-4*|phi-0,5|. Säge und Rechteck sind "naiv" (erzeugen Aliasing).
//  * Oktave und Verstimmung: f' = f * 2^(Oktave + Cent/1200). Eine Oktave verdoppelt die Frequenz,
//    100 Cent sind ein Halbton (Faktor 2^(1/12)).
//  * Tonhöhe pro Oszillator: f' = f * 2^(Oktave + Halbton/12 + Cent/1200).
//  * Symmetrie s (0..1, Mitte 0,5 = Grundform): Rechteck wird zum Puls mit Tastgrad s (PWM),
//    die anderen Formen bekommen eine geknickte Phase
//        phi' = 0,5*phi/s                    für phi < s
//        phi' = 0,5 + 0,5*(phi-s)/(1-s)      sonst
//    (Sinus wird schief, Dreieck kippt Richtung Säge, Säge bekommt zwei Steigungen).
//  * Sync: läuft Osc 1 über (phi1 springt von ~1 auf ~0), startet Osc 2 neu, und zwar mit dem
//    Bruchteil, den Osc 1 schon in die neue Periode gelaufen ist: phi2 = phi1 * f2/f1 (weniger Aliasing).
//  * FM von Osc 2 auf Osc 1 (Phasenmodulation): osc1 = w1(phi1 + 0,8*FM*osc2).
//    AM: osc1' = osc1 * (1 - AM + AM*osc2); bei AM = 1 ist das reine Ringmodulation (Summen-
//    und Differenzfrequenzen f1 +- f2).
//  * Free-run: die Phase wird beim Notenanschlag nicht auf 0 gesetzt, sondern startet irgendwo,
//    wie bei einem Oszillator, der ständig weiterläuft.
//  * Mix: y = 0,75 * ((1-m)*osc1 + m*osc2) + sub*0,5*sin(2*pi*phi_sub), phi_sub läuft bei f/2.
//    Spread verteilt Osc 1 nach links und Osc 2 nach rechts: Gewicht auf der Gegenseite = 1 - Spread.
//    Ab hier läuft die Kette in Stereo (zwei Filterkanäle, zwei Bitcrusher-Speicher).
//  * Hüllkurve ADSFR: Attack, Decay, Sustain, Fade, Release. Jede Phase läuft über x = 0..1 mit der Form
//        k(x) = (1 - e^(-c*x)) / (1 - e^(-c)),   c = 7 * Kurve   (Kurve 0 -> k(x) = x, linear)
//    Attack: v = v0 + (1 - v0) k(x); Decay: v = 1 + (S - 1) k(x); Release: v = v0 (1 - k(x)).
//    Fade: im Sustain sinkt der Pegel linear in "Fade" Sekunden auf 0 (Fade 0 = aus, Pegel bleibt).
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

        lowpass.prepare ({ sampleRate, (juce::uint32) blockSize, 2 });       // Stereo (Spread)
        lowpass.setType (juce::dsp::StateVariableTPTFilterType::lowpass);
        lowpass.setResonance (1.1f);

        clickFilter.prepare (spec);
        clickFilter.setType (juce::dsp::StateVariableTPTFilterType::bandpass);
        clickFilter.setResonance (6.0f);

        env.setSampleRate (sampleRate);
    }

    bool canPlaySound (juce::SynthesiserSound* s) override
    {
        return dynamic_cast<Mkvst04Sound*> (s) != nullptr;
    }

    void startNote (int midiNote, float velocity, juce::SynthesiserSound*, int) override
    {
        freq = (float) juce::MidiMessage::getMidiNoteInHertz (midiNote);
        level = velocity;
        phOsc1 = param ("osc1Free") > 0.5f ? rng.nextFloat() : 0.0f;
        phOsc2 = param ("osc2Free") > 0.5f ? rng.nextFloat() : 0.0f;
        phSub = phCar = phMod = 0.0f;
        clockPhase = 0.0f;
        clickEnv = 1.0f;                       // "Klack" beim Anschlagen
        lfoPhase = 0.0f;                       // LFO startet mit der Note
        shValue = rng.nextFloat() * 2.0f - 1.0f;
        holdCounter = 0;
        heldL = heldR = 0.0f;
        lowpass.reset();
        clickFilter.reset();
        updateEnv();
        env.noteOn();
    }

    void stopNote (float, bool allowTailOff) override
    {
        if (allowTailOff)
        {
            updateEnv();
            env.noteOff();
        }
        else
        {
            env.reset();
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
        const float mul1    = std::exp2 (std::round (param ("osc1Oct")) + std::round (param ("osc1Semi")) / 12.0f
                                         + param ("osc1Cent") / 1200.0f);
        const float mul2    = std::exp2 (std::round (param ("osc2Oct")) + std::round (param ("osc2Semi")) / 12.0f
                                         + param ("osc2Detune") / 1200.0f);
        const float sym1    = juce::jlimit (0.05f, 0.95f, param ("osc1Sym"));
        const float sym2    = juce::jlimit (0.05f, 0.95f, param ("osc2Sym"));
        const bool  sync    = param ("osc2Sync") > 0.5f;
        const float mix     = param ("oscMix");
        const float subAmt  = param ("sub");
        const float spread  = param ("oscSpread");
        const float fmAmt   = param ("oscFm");
        const float amAmt   = param ("oscAm");

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
        updateEnv();

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

            // Oszillatoren (Osc 2 zuerst, weil er Osc 1 per FM/AM moduliert)
            const float w2      = wave (wave2, phOsc2, sym2);
            float       w1      = wave (wave1, wrap (phOsc1 + 0.8f * fmAmt * w2), sym1);
            w1 *= 1.0f - amAmt + amAmt * w2;
            const float sub     = std::sin (twoPi * phSub);
            const float carrier = std::sin (twoPi * phCar + fmIndex * std::sin (twoPi * phMod));
            const float common  = subAmt * 0.5f * sub + mystery * 0.5f * carrier;
            const float oscL    = 0.75f * ((1.0f - mix) * w1 + mix * w2 * (1.0f - spread)) + common;
            const float oscR    = 0.75f * ((1.0f - mix) * w1 * (1.0f - spread) + mix * w2) + common;

            const float f    = freq * pitchMul;
            const float inc1 = f * mul1 / fs, inc2 = f * mul2 / fs;
            phOsc1 += inc1;
            phOsc2 = wrap (phOsc2 + inc2);
            if (phOsc1 >= 1.0f)
            {
                phOsc1 = wrap (phOsc1);
                if (sync) phOsc2 = wrap (phOsc1 * inc2 / inc1);
            }
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

            const float noise = rng.nextFloat() * 2.0f - 1.0f;
            const float click = clickFilter.processSample (0, noise) * clickEnv * klack * 1.5f;
            clickEnv *= clickDecay;

            const float dryL = lowpass.processSample (0, oscL) * gate + click;
            const float dryR = lowpass.processSample (1, oscR) * gate + click;

            // Knatter: Bitcrusher
            if (holdCounter <= 0)
            {
                heldL = std::floor (dryL * step + 0.5f) / step;
                heldR = std::floor (dryR * step + 0.5f) / step;
                holdCounter = hold;
            }
            --holdCounter;

            const float g  = env.next() * level * 0.35f * ampMod;
            const float sL = std::tanh ((dryL + knatter * (heldL - dryL)) * 1.2f) * g;
            const float sR = std::tanh ((dryR + knatter * (heldR - dryR)) * 1.2f) * g;

            if (out.getNumChannels() == 1)
                out.addSample (0, startSample + i, 0.5f * (sL + sR));
            else
            {
                out.addSample (0, startSample + i, sL);
                out.addSample (1, startSample + i, sR);
            }
        }

        if (! env.isActive())
            clearCurrentNote();
    }

private:
    static float wrap (float p) { return p - std::floor (p); }
    float param (const char* id) const { return apvts.getRawParameterValue (id)->load(); }

    // 0 Sinus, 1 Säge, 2 Rechteck, 3 Dreieck; s = Symmetrie (0,5 = Grundform)
    static float wave (int type, float p, float s)
    {
        if (type == 2)
            return p < s ? 1.0f : -1.0f;
        p = p < s ? 0.5f * p / s : 0.5f + 0.5f * (p - s) / (1.0f - s);
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

    void updateEnv()
    {
        env.set (param ("attack"), param ("envDecay"), param ("envSustain"), param ("envFade"),
                 param ("release"), param ("envCurve"));
    }

    // Hüllkurve ADSFR mit einstellbarer Kurvenform (Formeln oben). Startet bei einem neuen Anschlag
    // vom aktuellen Pegel aus, damit es nicht knackt.
    struct Envelope
    {
        void setSampleRate (double r) { fs = (float) r; }
        void set (float a, float d, float s, float f, float r, float curve)
        {
            att = a; dec = d; sus = s; fade = f; rel = r; c = 7.0f * curve;
        }
        void noteOn()  { stage = 1; x = 0.0f; v0 = v; }
        void noteOff() { if (stage != 0) { stage = 4; x = 0.0f; v0 = v; } }
        void reset()   { stage = 0; v = 0.0f; }
        bool isActive() const { return stage != 0; }

        float shape (float t) const
        {
            return c < 0.01f ? t : (1.0f - std::exp (-c * t)) / (1.0f - std::exp (-c));
        }
        bool advance (float seconds)
        {
            x += 1.0f / (juce::jmax (0.001f, seconds) * fs);
            return x >= 1.0f;
        }
        float next()
        {
            switch (stage)
            {
                case 1:                                   // Attack
                    if (advance (att)) { v = 1.0f; stage = 2; x = 0.0f; }
                    else v = v0 + (1.0f - v0) * shape (x);
                    break;
                case 2:                                   // Decay
                    if (advance (dec)) { v = sus; stage = 3; fadeGain = 1.0f; }
                    else v = 1.0f + (sus - 1.0f) * shape (x);
                    break;
                case 3:                                   // Sustain mit Fade
                    if (fade > 0.001f)
                    {
                        fadeGain -= 1.0f / (fade * fs);
                        if (fadeGain <= 0.0f) { fadeGain = 0.0f; stage = 0; }
                    }
                    v = sus * fadeGain;
                    break;
                case 4:                                   // Release
                    if (advance (rel)) { v = 0.0f; stage = 0; }
                    else v = v0 * (1.0f - shape (x));
                    break;
                default: v = 0.0f; break;
            }
            return v;
        }

        float fs = 44100.0f, att = 0.02f, dec = 0.5f, sus = 0.7f, fade = 0.0f, rel = 0.8f, c = 0.0f;
        float v = 0.0f, v0 = 0.0f, x = 0.0f, fadeGain = 1.0f;
        int stage = 0;
    };

    juce::AudioProcessorValueTreeState& apvts;
    juce::dsp::StateVariableTPTFilter<float> lowpass, clickFilter;
    Envelope env;
    juce::Random rng;

    double sr = 44100.0;
    float freq = 440.0f, level = 1.0f;
    float phOsc1 = 0.0f, phOsc2 = 0.0f, phSub = 0.0f, phCar = 0.0f, phMod = 0.0f;
    float clockPhase = 0.0f, clickEnv = 0.0f, lfoPhase = 0.0f, shValue = 0.0f, heldL = 0.0f, heldR = 0.0f;
    int holdCounter = 0;
};
