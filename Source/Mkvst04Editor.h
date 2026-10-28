#pragma once
#include "Mkvst04Processor.h"

// Regler: Metallkappe mit leuchtendem Wertebogen (Teal nach Amber). Bei Bedienung (Ziehen, Automation,
// Mutation) bekommt der Regler einen Lichthof. Der Wert steht in der Property "glow" (0..1).
class Mkvst04LookAndFeel : public juce::LookAndFeel_V4
{
public:
    Mkvst04LookAndFeel();
    void drawRotarySlider (juce::Graphics&, int x, int y, int w, int h, float sliderPos,
                           float startAngle, float endAngle, juce::Slider&) override;
};

// Oberfläche im Stil eines düsteren Miniatur-Szenarios: Zwei riesige schwarze Figuren mit gemeinsamem Kopf
// stehen hinter einer nächtlichen Skyline; in jedem Kopf pocht ein Gehirn im Takt der DAW.
// Jede der 36 Noten (C2..B4) ist ein Hochhaus. Links über der Skyline sitzen die Oszillatoren, rechts der
// LFO; unten Filter, Effekte und Hüllkurve. Beim Spielen zucken Blitze aus den Gehirnen zum Dach des Hauses.
// Klick auf eine Figur = Klang mutieren.
class Mkvst04Editor : public juce::AudioProcessorEditor, private juce::Timer
{
public:
    explicit Mkvst04Editor (Mkvst04Processor&);
    ~Mkvst04Editor() override;

    void paint (juce::Graphics&) override;
    void resized() override;
    void mouseDown (const juce::MouseEvent&) override;
    void mouseDrag (const juce::MouseEvent&) override;
    void mouseUp (const juce::MouseEvent&) override;

private:
    static constexpr int firstNote = 36;      // C2
    static constexpr int numKeys   = 36;      // 3 Oktaven
    static constexpr int numKnobs  = 27;      // 9 unten, 5 Oszillatoren, 4 LFO, 5 Echo, 4 Reverb

    struct Knob
    {
        juce::Slider slider;
        juce::Label label;
        std::unique_ptr<juce::AudioProcessorValueTreeState::SliderAttachment> attachment;
    };
    struct Selector                           // Wellenform-Wähler, selbst gezeichnet
    {
        const char* id;
        int count;
        int kind;                             // 0 Oszillator, 1 LFO (andere Reihenfolge der Formen)
        int group;                            // 1 Oszillatoren, 2 LFO (für die Aktivitäts-LED)
        juce::Rectangle<float> r;
        float glow = 0.0f;
    };
    struct Flash   { int key; float life; };
    struct Dust    { float x, y, vx, vy, phase; };
    struct Spore   { juce::Point<float> p, v; float life, maxLife, size; juce::Colour col; };
    struct Sparkle { juce::Point<float> p, v; float life, maxLife; };

    void timerCallback() override;
    int  keyAt (juce::Point<float>, bool columnOnly) const;
    juce::Rectangle<float> keyRect (int key) const;     // Haus + Beschriftungsleiste
    juce::Point<float> roofPoint (int key) const;       // Blitz-Ziel: Dach
    float param (const char* id) const;
    void setChoice (const char* id, int index, int count);
    void mutate();
    void buildFigures();
    void buildSkyline();
    void buildGrain();
    void buildFarCity();
    void spawnSpores (int key, int count);
    void spawnSparkles (juce::Point<float> at, int count);

    void drawBackground (juce::Graphics&);
    void drawFarCity (juce::Graphics&);
    void drawFigures (juce::Graphics&);
    void drawBrain (juce::Graphics&, const juce::AffineTransform& tfHead, bool mirrored, float flash);
    float brainGlow (float flash) const;
    void drawSkyline (juce::Graphics&);
    void drawFog (juce::Graphics&);
    void drawPanels (juce::Graphics&);
    void drawFxPanels (juce::Graphics&);
    void drawScope (juce::Graphics&, juce::Rectangle<float> r, int type, float cycles, float phase,
                    float amp, float glow, juce::Colour c);
    void drawSelector (juce::Graphics&, const Selector&);
    void drawEffects (juce::Graphics&);
    void drawTitle (juce::Graphics&);

    Mkvst04Processor& proc;
    Mkvst04LookAndFeel laf;

    std::array<Knob, numKnobs> knobs;
    std::array<float, numKnobs> knobGlow {};
    std::array<float, 5> activity {};                   // Aktivitäts-LEDs: unten / Osc / LFO / Echo / Reverb
    std::array<float, numKnobs> knobRipple {}, knobSpin {};          // Wellenring bei Wertänderung
    std::array<Selector, 3> selectors { { { "osc1Wave", 4, 0, 1, {}, 0.0f },
                                          { "osc2Wave", 4, 0, 1, {}, 0.0f },
                                          { "lfoShape", 5, 1, 2, {}, 0.0f } } };

    // Layout
    juce::Point<float> sceneCentre;
    juce::Rectangle<float> echoPanel, voidPanel;
    juce::Rectangle<float> oscPanel, lfoPanel, osc1Scope, osc2Scope, lfoScope;
    std::array<juce::Point<float>, 3> routeLed {};      // LEDs Filter / Pitch / Amp im LFO-Feld

    // Figuren (Maße in "Figureneinheiten", werden mit figureScale vergrößert)
    juce::Path headPath, bodyL, bodyR;
    juce::Path farBuildings, farWindows;                // ferne Stadt (Kulisse)
    juce::Image grain;                                  // Filmkorn
    juce::Path figureMask;                              // aktuelle Umrisse beider Figuren (pro Frame)
    juce::Path brainShape, brainFolds;                  // Gehirn im linken Kopf (rechts gespiegelt)
    const juce::Point<float> brainCentreL { -56.3f, -117.0f };
    juce::Point<float> brainWorldL, brainWorldR;        // Blitz-Ursprünge: die Gehirne

    // Skyline
    std::array<float, numKeys> bldHeight {}, stepH {};
    std::array<bool, numKeys> hasAntenna {}, hasStep {}, hasTank {};

    // Zustand und Effekte
    std::vector<Flash> flashes;
    std::vector<Dust> dust;
    std::vector<Spore> spores;
    std::vector<Sparkle> sparkles;
    std::array<bool, numKeys> keyOn {};
    std::array<float, numKeys> keyGlow {};
    float level = 0.0f, time = 0.0f;
    float kickL = 0.0f, kickR = 0.0f, kickHead = 0.0f, glitch = 0.0f, skyFlash = 0.0f;
    float headFlashL = 0.0f, headFlashR = 0.0f;           // Gehirne leuchten bei Noten ihrer Seite auf
    float beatPhase = 0.0f, brainPulse = 0.0f;            // Phase im Viertel (0..1) und Puls daraus
    int heldKey = -1;
    juce::Random rng;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (Mkvst04Editor)
};
