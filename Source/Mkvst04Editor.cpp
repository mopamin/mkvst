#include "Mkvst04Editor.h"
#include <algorithm>

namespace
{
    constexpr int   winW        = 1200;
    constexpr int   winH        = 990;
    constexpr float figureScale = 1.45f;      // Figuren sind 1,45x so groß wie ihre Maße im Code
    constexpr float skyX0       = 40.0f;      // Skyline: linker Rand
    constexpr float skyW        = 1120.0f;    // Skyline: Gesamtbreite (36 Häuser)
    constexpr float keyW        = skyW / 36.0f;
    constexpr float barTop      = 650.0f;     // Oberkante des Balkens = Boden der Häuser
    constexpr float barH        = 34.0f;      // Höhe der Beschriftungsleiste
    constexpr int   panelTop    = 716;        // Beginn der unteren Regler-Leiste
    constexpr float twoPi       = juce::MathConstants<float>::twoPi;
    constexpr float pi          = juce::MathConstants<float>::pi;

    // Palette nach dem Referenzbild: dunkles Teal mit Nebel, Silbermetall (Regler),
    // warme Fensterlichter, bunte LEDs, elektrisch blaue Blitze, rosa Gehirne
    const juce::Colour skyTop     (0xff050f13);
    const juce::Colour skyMid     (0xff0b2228);
    const juce::Colour skyLow     (0xff16393c);
    const juce::Colour fogTeal    (0xff2a6a6a);
    const juce::Colour metalLight (0xffc3c8cc);
    const juce::Colour metalMid   (0xff7d8489);
    const juce::Colour metalDark  (0xff2c3236);
    const juce::Colour ledGreen   (0xff4cff6a);
    const juce::Colour ledYellow  (0xffffd84a);
    const juce::Colour ledRed     (0xffff4040);
    const juce::Colour windowWarm (0xfff4d58a);
    const juce::Colour teal       (0xff35d0c0);
    const juce::Colour amber      (0xffffb040);
    const juce::Colour electric   (0xff6fd8ff);
    const juce::Colour textCol    (0xff9fc4c8);
    const juce::Colour steelLine  (0xff4a555b);
    const juce::Colour brainPink  (0xffff6ec0);     // Gehirn: sanftes Rosa
    const juce::Colour brainLight (0xffffc4e8);     // Windungen, heller

    struct KnobInfo { const char* id; const char* name; const char* suffix; int group; int decimals; };
    const KnobInfo knobInfo[] =
    {
        // Gruppe 0: unten (Filter, Effekte, Hüllkurve)
        { "dark", "Dunkel", "", 0, 2 },        { "klack", "Klack", "", 0, 2 },
        { "knatter", "Knatter", "", 0, 2 },    { "mystery", "Mystik", "", 0, 2 },
        { "clockRate", "Uhr-Takt", " Hz", 0, 1 }, { "clockDepth", "Uhr-Tiefe", "", 0, 2 },
        { "attack", "Attack", " s", 0, 2 },    { "release", "Release", " s", 0, 2 },
        { "gain", "Gain", "", 0, 2 },
        // Gruppe 1: Oszillatoren
        { "osc1Oct", "Osc1 Okt", "", 1, 0 },   { "osc2Oct", "Osc2 Okt", "", 1, 0 },
        { "osc2Detune", "Detune", " ct", 1, 0 }, { "oscMix", "Mix", "", 1, 2 },
        { "sub", "Sub", "", 1, 2 },
        // Gruppe 2: LFO
        { "lfoRate", "Rate", " Hz", 2, 2 },    { "lfoFilter", "Filter", "", 2, 2 },
        { "lfoPitch", "Pitch", "", 2, 2 },     { "lfoAmp", "Amp", "", 2, 2 },
        // Gruppe 3: Dark Echo
        { "echoTime", "Time", " ms", 3, 0 },   { "echoFb", "Feedback", "", 3, 2 },
        { "echoDark", "Dark", "", 3, 2 },      { "echoDrift", "Drift", "", 3, 2 },
        { "echoMix", "Mix", "", 3, 2 },
        // Gruppe 4: Void Reverb
        { "revSize", "Size", "", 4, 2 },       { "revDamp", "Damp", "", 4, 2 },
        { "revSpook", "Spook", "", 4, 2 },     { "revMix", "Mix", "", 4, 2 }
    };

    bool isBlackKey (int note)
    {
        const int pc = note % 12;
        return pc == 1 || pc == 3 || pc == 6 || pc == 8 || pc == 10;
    }

    // Hausfarbe beim Spielen: tiefe Töne warm (Amber), hohe kühl (Blau), dazwischen grünlich wie die LEDs
    juce::Colour noteColour (int note)
    {
        const float p = (float) (note - 36) / 35.0f;
        return juce::Colour::fromHSV (0.11f + p * 0.45f, 0.65f, 1.0f, 1.0f);
    }

    // Blitzfarbe: elektrisches Blau. Der Unterschied nach Tonhöhe ist bewusst klein:
    // tief = etwas dunkleres Blau (Hue 0,63), hoch = helleres Eisblau (Hue 0,56).
    juce::Colour boltColour (float pitch01, float saturation)
    {
        return juce::Colour::fromHSV (juce::jmap (pitch01, 0.63f, 0.56f), saturation, 1.0f, 1.0f);
    }

    // Puls pro Schlag (f = Phase im Viertel, 0..1): kurzer Anstieg, dann weiches Abklingen.
    //     f < 0,05:  p = f / 0,05      sonst:  p = exp(-4,5 (f - 0,05))
    float beatPulse (float f)
    {
        return f < 0.05f ? f / 0.05f : std::exp (-4.5f * (f - 0.05f));
    }

    // Einfacher Integer-Hash (stabile "Zufalls"-Werte für Fenster, Terminal-Zeilen, S&H)
    juce::uint32 hash32 (juce::uint32 h)
    {
        h ^= h >> 16;  h *= 0x7feb352dU;
        h ^= h >> 15;  h *= 0x846ca68bU;
        h ^= h >> 16;
        return h;
    }

    // Wellenform aus der Phase: 0 Sinus, 1 Säge, 2 Rechteck, 3 Dreieck, 4 Sample & Hold (8 Stufen/Periode)
    float waveAt (int type, float p, int seed)
    {
        p -= std::floor (p);
        switch (type)
        {
            case 0:  return std::sin (twoPi * p);
            case 1:  return 2.0f * p - 1.0f;
            case 2:  return p < 0.5f ? 1.0f : -1.0f;
            case 3:  return 1.0f - 4.0f * std::abs (p - 0.5f);
            default: return (float) (hash32 ((juce::uint32) (seed * 131 + (int) (p * 8.0f))) & 1023u) / 511.5f - 1.0f;
        }
    }

    juce::Path wavePath (int type, juce::Rectangle<float> r, float cycles, float phase, float amp, int seed)
    {
        juce::Path p;
        const int n = juce::jmax (24, (int) r.getWidth());
        for (int i = 0; i <= n; ++i)
        {
            const float t = (float) i / (float) n;
            const float y = r.getCentreY() - waveAt (type, cycles * t + phase, seed) * amp * r.getHeight() * 0.45f;
            if (i == 0) p.startNewSubPath (r.getX(), y); else p.lineTo (r.getX() + r.getWidth() * t, y);
        }
        return p;
    }

    // Gezackter Blitz von a nach b. Die Zwischenpunkte liegen auf der Geraden und werden
    // senkrecht verschoben; die Verschiebung ist zufällig und wird zu den Enden hin
    // mit sin(pi*t) auf null abgeschwächt, damit der Blitz exakt an a und b ansetzt.
    juce::Path makeBolt (juce::Point<float> a, juce::Point<float> b, juce::int64 seed,
                         float jitter, float segLen, int branches)
    {
        juce::Random r (seed);
        const auto d = b - a;
        const float len = juce::jmax (1.0f, std::hypot (d.x, d.y));
        const juce::Point<float> u (d.x / len, d.y / len);
        const juce::Point<float> n (-u.y, u.x);
        const int segs = juce::jmax (6, (int) (len / segLen));

        std::vector<juce::Point<float>> pts;
        pts.push_back (a);
        for (int i = 1; i < segs; ++i)
        {
            const float t = (float) i / (float) segs;
            const float off = (r.nextFloat() - 0.5f) * 2.0f * jitter * std::sin (pi * t);
            pts.push_back (a + d * t + n * off);
        }
        pts.push_back (b);

        juce::Path p;
        p.startNewSubPath (pts.front());
        for (size_t i = 1; i < pts.size(); ++i)
            p.lineTo (pts[i]);

        // Seitenäste mit kleinem Knick
        for (int k = 0; k < branches; ++k)
        {
            const size_t idx = (size_t) juce::jlimit (1, segs - 1, (segs * (k + 1)) / (branches + 1));
            const auto start = pts[idx];
            const float ang = r.nextBool() ? 0.7f : -0.7f;
            const auto dir = u.transformedBy (juce::AffineTransform::rotation (ang));
            const auto knick = start + dir * (16.0f + r.nextFloat() * 12.0f) + n * ((r.nextFloat() - 0.5f) * 10.0f);
            const auto ende  = start + dir * (34.0f + r.nextFloat() * 26.0f);
            p.startNewSubPath (start);
            p.lineTo (knick);
            p.lineTo (ende);
        }
        return p;
    }

    // Vier Striche übereinander: breiter weicher Schein, Schein, Farbe, fast weißer Kern.
    // pitch01: 0 = tiefste Note, 1 = höchste. Tiefe Töne: dicker und ruhiger, hohe: feiner und zackiger.
    void drawBolt (juce::Graphics& g, juce::Point<float> a, juce::Point<float> b,
                   juce::int64 seed, float intensity, float pitch01, juce::Colour c)
    {
        const auto p = makeBolt (a, b, seed,
                                 juce::jmap (pitch01, 26.0f, 18.0f),
                                 juce::jmap (pitch01, 20.0f, 12.0f),
                                 pitch01 > 0.5f ? 2 : 1);
        auto stroke = [&] (float w, juce::Colour col, float alpha)
        {
            g.setColour (col.withAlpha (juce::jlimit (0.0f, 1.0f, alpha)));
            g.strokePath (p, juce::PathStrokeType (w, juce::PathStrokeType::mitered, juce::PathStrokeType::rounded));
        };
        const float glowW = juce::jmap (pitch01, 12.0f, 9.0f);
        stroke (glowW * 1.8f, c, 0.05f * intensity);
        stroke (glowW,        c, 0.14f * intensity);
        stroke (juce::jmap (pitch01, 5.5f, 4.0f), c.interpolatedWith (juce::Colours::white, 0.35f), 0.5f * intensity);
        stroke (juce::jmap (pitch01, 2.0f, 1.4f), c.interpolatedWith (juce::Colours::white, 0.85f), 0.98f * intensity);
    }
}

//==============================================================================
Mkvst04LookAndFeel::Mkvst04LookAndFeel()
{
    setColour (juce::Slider::textBoxTextColourId, juce::Colour (0xffc9dfe2));
    setColour (juce::Slider::textBoxOutlineColourId, juce::Colours::transparentBlack);
    setColour (juce::Slider::textBoxBackgroundColourId, juce::Colours::transparentBlack);
}

// Animierter Regler. Eigenschaften (vom Editor pro Frame gesetzt): glow 0..1 (Bedienung), ripple 0..1
// (Wellenring nach einer Änderung), spin (Drehwinkel des Punktrings), t (Zeit in Sekunden).
//  - Außen kreist ein Punktring, schneller bei hohem Wert und bei Bedienung.
//  - 11 LED-Striche entlang des Bogens leuchten bis zum aktuellen Wert und flackern bei Bedienung.
//  - Der Wertebogen hat einen leuchtenden Kopf mit Kometenschweif.
//  - Bei jeder Änderung läuft ein Wellenring von der Mitte nach außen.
//  - Die Metallkappe wird beim Anfassen leicht eingedrückt und bekommt einen farbigen Rand.
void Mkvst04LookAndFeel::drawRotarySlider (juce::Graphics& g, int x, int y, int w, int h, float pos,
                                           float a0, float a1, juce::Slider& slider)
{
    const auto& props = slider.getProperties();
    const float glow   = (float) props.getWithDefault ("glow", 0.0f);
    const float ripple = (float) props.getWithDefault ("ripple", 0.0f);
    const float spin   = (float) props.getWithDefault ("spin", 0.0f);
    const float t      = (float) props.getWithDefault ("t", 0.0f);

    auto b = juce::Rectangle<float> ((float) x, (float) y, (float) w, (float) h).reduced (4.0f);
    const float d = juce::jmin (b.getWidth(), b.getHeight());
    auto r = juce::Rectangle<float> (d, d).withCentre (b.getCentre());
    const auto c = r.getCentre();
    const float R = d * 0.5f;
    const float angle = a0 + pos * (a1 - a0);
    const auto col = teal.interpolatedWith (amber, pos);
    const float tw = juce::MathConstants<float>::twoPi;

    auto onCircle = [&] (float rad, float ang) { return juce::Point<float> (c.x + std::sin (ang) * rad, c.y - std::cos (ang) * rad); };

    // Lichthof bei Bedienung, im Ruhezustand ein leises Atmen
    {
        const float a = 0.5f * glow + 0.05f * (0.5f + 0.5f * std::sin (t * 1.6f + pos * 5.0f));
        juce::ColourGradient halo (col.withAlpha (a), c, juce::Colours::transparentBlack, c.translated (d * 0.8f, 0.0f), true);
        g.setGradientFill (halo);
        g.fillEllipse (r.expanded (d * 0.3f));
    }

    // kreisender Punktring
    for (int k = 0; k < 14; ++k)
    {
        const float a = spin + (float) k * tw / 14.0f;
        const float al = 0.12f + 0.4f * (0.5f + 0.5f * std::sin (a * 3.0f + t)) + 0.35f * glow;
        g.setColour (col.withAlpha (juce::jlimit (0.0f, 1.0f, al)));
        const auto p = onCircle (R + 1.0f, a);
        g.fillEllipse (p.x - 1.0f, p.y - 1.0f, 2.0f, 2.0f);
    }

    g.setColour (juce::Colour (0xff0b0f11));
    g.fillEllipse (r.reduced (2.0f));

    // Wertebogen
    const float rad = R - 4.5f;
    juce::Path track, value;
    track.addCentredArc (c.x, c.y, rad, rad, 0.0f, a0, a1, true);
    value.addCentredArc (c.x, c.y, rad, rad, 0.0f, a0, angle, true);
    g.setColour (juce::Colour (0xff1f2a2e));
    g.strokePath (track, juce::PathStrokeType (3.0f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
    g.setColour (col.withAlpha (0.2f + 0.4f * glow));
    g.strokePath (value, juce::PathStrokeType (7.0f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
    g.setColour (col.brighter (0.2f * glow));
    g.strokePath (value, juce::PathStrokeType (3.0f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));

    // LED-Striche
    for (int k = 0; k <= 10; ++k)
    {
        const float f = (float) k / 10.0f;
        const float a = a0 + f * (a1 - a0);
        const bool lit = f <= pos + 0.001f;
        const float flick = lit ? 0.6f + 0.4f * glow * std::sin (t * 14.0f + (float) k * 1.7f) : 0.0f;
        g.setColour (lit ? col.withAlpha (juce::jlimit (0.0f, 1.0f, 0.55f + flick * 0.5f)) : juce::Colour (0xff2a373b));
        g.drawLine (juce::Line<float> (onCircle (R * 0.70f, a), onCircle (R * 0.79f, a)), 1.4f);
    }

    // Kometenschweif und Kopf
    for (int j = 5; j >= 1; --j)
    {
        const float a = angle - (float) j * 0.11f;
        if (a < a0) continue;
        const auto p = onCircle (rad, a);
        g.setColour (col.withAlpha ((0.5f - 0.09f * (float) j) * (0.3f + 0.7f * glow)));
        g.fillEllipse (juce::Rectangle<float> (3.5f, 3.5f).withCentre (p));
    }
    {
        const auto head = onCircle (rad, angle);
        g.setColour (col.withAlpha (0.25f + 0.3f * glow));
        g.fillEllipse (juce::Rectangle<float> (9.0f + 5.0f * glow, 9.0f + 5.0f * glow).withCentre (head));
        g.setColour (juce::Colours::white.withAlpha (0.85f));
        g.fillEllipse (juce::Rectangle<float> (3.2f, 3.2f).withCentre (head));
    }

    // Wellenring nach einer Änderung
    if (ripple > 0.02f)
    {
        const float rr = R * (0.45f + 0.5f * (1.0f - ripple));
        g.setColour (col.withAlpha (ripple * 0.7f));
        g.drawEllipse (juce::Rectangle<float> (rr * 2.0f, rr * 2.0f).withCentre (c), 1.6f);
    }

    // Knopfkappe aus gebürstetem Metall, beim Anfassen leicht eingedrückt
    const auto cap = juce::Rectangle<float> (R * 1.3f, R * 1.3f).withCentre (c).reduced (R * 0.06f * glow);
    juce::ColourGradient metal (metalLight, cap.getX(), cap.getY(), metalDark, cap.getRight(), cap.getBottom(), false);
    metal.addColour (0.5, metalMid);
    g.setGradientFill (metal);
    g.fillEllipse (cap);
    g.setColour (col.withAlpha (0.25f + 0.6f * glow));
    g.drawEllipse (cap, 1.2f);
    g.setColour (juce::Colours::white.withAlpha (0.25f));
    g.fillEllipse (juce::Rectangle<float> (cap.getWidth() * 0.35f, cap.getHeight() * 0.2f)
                       .withCentre (cap.getCentre().translated (-cap.getWidth() * 0.15f, -cap.getHeight() * 0.25f)));

    // Zeiger
    juce::Path pointer;
    pointer.addRoundedRectangle (-1.5f, -cap.getHeight() * 0.5f + 2.0f, 3.0f, cap.getHeight() * 0.3f, 1.5f);
    g.setColour (amber.brighter (0.2f + 0.5f * glow));
    g.fillPath (pointer, juce::AffineTransform::rotation (angle).translated (c));
}

//==============================================================================
Mkvst04Editor::Mkvst04Editor (Mkvst04Processor& p)
    : AudioProcessorEditor (&p), proc (p)
{
    static_assert (sizeof (knobInfo) / sizeof (knobInfo[0]) == (size_t) numKnobs, "Regler-Tabelle prüfen");
    static_assert (numKeys == 36, "keyW ist für 36 Tasten berechnet");
    setLookAndFeel (&laf);

    for (size_t i = 0; i < knobs.size(); ++i)
    {
        auto& k = knobs[i];
        const auto& info = knobInfo[i];

        k.slider.setSliderStyle (juce::Slider::RotaryHorizontalVerticalDrag);
        k.slider.setRotaryParameters (pi * 1.2f, pi * 2.8f, true);
        k.slider.setTextBoxStyle (juce::Slider::TextBoxBelow, false, 60, 14);
        k.slider.setNumDecimalPlacesToDisplay (info.decimals);
        k.slider.setTextValueSuffix (info.suffix);
        addAndMakeVisible (k.slider);

        k.label.setText (info.name, juce::dontSendNotification);
        k.label.setJustificationType (juce::Justification::centred);
        k.label.setFont (juce::FontOptions (12.0f));
        k.label.setColour (juce::Label::textColourId, textCol);
        addAndMakeVisible (k.label);

        k.attachment = std::make_unique<juce::AudioProcessorValueTreeState::SliderAttachment> (
            proc.apvts, info.id, k.slider);

        // Leuchten bei Bedienung: auch bei Automation und Mutation (erst nach dem Anlegen des Attachments)
        k.slider.onValueChange = [this, i]
        {
            knobGlow[i] = 1.0f;
            knobRipple[i] = 1.0f;
            activity[(size_t) knobInfo[i].group] = 1.0f;
        };
    }

    buildFigures();
    buildSkyline();
    buildGrain();
    buildFarCity();

    for (int i = 0; i < 70; ++i)
        dust.push_back ({ rng.nextFloat() * (float) winW, rng.nextFloat() * (float) winH,
                          (rng.nextFloat() - 0.5f) * 6.0f, -(4.0f + rng.nextFloat() * 10.0f),
                          rng.nextFloat() * twoPi });

    setSize (winW, winH);
    startTimerHz (60);
}

Mkvst04Editor::~Mkvst04Editor()
{
    stopTimer();
    if (heldKey >= 0)
        proc.keyboardState.noteOff (1, firstNote + heldKey, 0.0f);
    setLookAndFeel (nullptr);
}

//==============================================================================
// Zwei Piktogramm-Figuren mit gemeinsamem Kopf (Maße aus dem Referenzbild ausgemessen,
// Nullpunkt = Mitte der Figurengruppe). Die Kopf-Blöcke sitzen mittig über den Körpern.
void Mkvst04Editor::buildFigures()
{
    headPath.addRoundedRectangle (-91.1f, -154.1f, 69.6f, 73.8f, 27.5f);
    headPath.addRoundedRectangle ( 21.5f, -154.1f, 69.6f, 73.8f, 27.5f);
    headPath.addRectangle (-30.0f, -142.4f, 60.0f, 48.8f);

    bodyL.addRoundedRectangle (-111.4f, -86.0f, 108.9f, 147.0f, 42.7f);
    bodyL.addRectangle (-84.0f, 50.0f, 56.0f, 104.6f);
    bodyR.addRoundedRectangle (2.5f, -86.0f, 108.9f, 147.0f, 42.7f);
    bodyR.addRectangle (28.0f, 50.0f, 56.0f, 104.6f);

    // Gehirn im linken Kopf, Frontansicht: zwei Hälften nebeneinander, unten außen die Schläfenlappen,
    // in der Mitte der Hirnstamm. Jede Hälfte ist eine Superellipse (kantiger als eine Ellipse, damit die
    // Innenseiten am Mittelspalt flach aneinanderliegen):
    //     x(a) = ±11 + 11,5 sgn(cos a) |cos a|^(2/n),   y(a) = -2 + 13,5 sgn(sin a) |sin a|^(2/n),   n = 2,6
    const auto c = brainCentreL;
    auto sgnPow = [] (float v) { return std::copysign (std::pow (std::abs (v), 2.0f / 2.6f), v); };
    for (float side : { -1.0f, 1.0f })
    {
        juce::Path half;
        for (int k = 0; k < 96; ++k)
        {
            const float a = (float) k / 96.0f * twoPi;
            const float x = c.x + side * 11.0f + 11.5f * sgnPow (std::cos (a));
            const float y = c.y - 2.0f + 13.5f * sgnPow (std::sin (a));
            if (k == 0) half.startNewSubPath (x, y);
            else        half.lineTo (x, y);
        }
        half.closeSubPath();
        brainShape.addPath (half);
        brainShape.addEllipse (c.x + side * 14.0f - 6.5f, c.y + 2.0f, 13.0f, 10.0f);   // Schläfenlappen
    }
    brainShape.addRoundedRectangle (c.x - 2.5f, c.y + 8.0f, 5.0f, 11.0f, 2.5f);          // Hirnstamm

    // Mittelspalt zwischen den Hälften (wird dunkel gezeichnet)
    brainFissure.startNewSubPath (c.x, c.y - 15.5f);
    brainFissure.lineTo (c.x, c.y + 9.0f);

    // Windungen je Hälfte: drei offene, wellige Bögen von oben innen über außen nach unten,
    //     r(t) = f + 0,08 sin((9 - 2k) pi t + k) + 0,04 sin((17 - 3k) pi t + 2k),   a(t) = -60° - 190° t
    // dazu eine wellige Linie am Mittelspalt und die Furche über dem Schläfenlappen.
    for (float side : { -1.0f, 1.0f })
    {
        auto X = [&] (float xl) { return c.x - side * xl; };          // xl: Koordinate der linken Hälfte
        for (int k = 0; k < 3; ++k)
        {
            const float f = 0.8f - 0.25f * (float) k, kf = (float) k;
            for (int n = 0; n <= 80; ++n)
            {
                const float t = (float) n / 80.0f;
                const float a = juce::degreesToRadians (-60.0f - 190.0f * t);
                const float r = f + 0.08f * std::sin (t * (9.0f - 2.0f * kf) * pi + kf)
                                  + 0.04f * std::sin (t * (17.0f - 3.0f * kf) * pi + 2.0f * kf);
                const float x = X (-11.0f + 11.5f * r * std::cos (a));
                const float y = c.y - 2.0f + 13.5f * r * std::sin (a);
                if (n == 0) brainFolds.startNewSubPath (x, y);
                else        brainFolds.lineTo (x, y);
            }
        }
        for (int n = 0; n <= 30; ++n)
        {
            const float t = (float) n / 30.0f;
            const float x = c.x + side * (3.5f + 0.9f * std::sin (t * 11.0f));
            if (n == 0) brainFolds.startNewSubPath (x, c.y - 12.0f + 18.0f * t);
            else        brainFolds.lineTo (x, c.y - 12.0f + 18.0f * t);
        }
        for (int n = 0; n <= 30; ++n)
        {
            const float t = (float) n / 30.0f;
            const float x = c.x + side * (9.0f + 8.0f * t), y = c.y + 7.0f + 1.2f * std::sin (t * 12.0f);
            if (n == 0) brainFolds.startNewSubPath (x, y);
            else        brainFolds.lineTo (x, y);
        }
    }
}

// Skyline: Jede Note ist ein Haus. C-Töne sind höhere Türme mit Antenne, Halbtöne etwas niedriger
// und dunkler (so erkennt man die Klaviertasten wieder). Fester Zufalls-Startwert.
void Mkvst04Editor::buildSkyline()
{
    juce::Random r (31337);
    for (int i = 0; i < numKeys; ++i)
    {
        const int note = firstNote + i;
        const float rh   = r.nextFloat();
        const float rStep = r.nextFloat();
        const float rStepH = r.nextFloat();
        const float rAnt = r.nextFloat();
        const float rTank = r.nextFloat();

        float h = 64.0f + 110.0f * std::pow (rh, 1.2f);
        if (note % 12 == 0) h += 18.0f;
        if (isBlackKey (note)) h *= 0.82f;
        bldHeight[(size_t) i]  = h;
        hasStep[(size_t) i]    = rStep < 0.3f;
        stepH[(size_t) i]      = hasStep[(size_t) i] ? 10.0f + rStepH * 6.0f : 0.0f;
        hasAntenna[(size_t) i] = (note % 12 == 0) || rAnt < 0.15f;
        hasTank[(size_t) i]    = ! hasAntenna[(size_t) i] && rTank < 0.3f;
    }
}

//==============================================================================
void Mkvst04Editor::resized()
{
    sceneCentre = { (float) getWidth() * 0.5f, 290.0f };

    // Panels links (Oszillatoren) und rechts (LFO) über der Skyline
    oscPanel = juce::Rectangle<float> (30.0f, 84.0f, 380.0f, 322.0f);
    lfoPanel = juce::Rectangle<float> ((float) getWidth() - 410.0f, 84.0f, 380.0f, 322.0f);

    osc1Scope = juce::Rectangle<float> (oscPanel.getX() + 12.0f,  oscPanel.getY() + 34.0f, 170.0f, 78.0f);
    osc2Scope = juce::Rectangle<float> (oscPanel.getX() + 198.0f, oscPanel.getY() + 34.0f, 170.0f, 78.0f);
    selectors[0].r = juce::Rectangle<float> (oscPanel.getX() + 12.0f,  oscPanel.getY() + 118.0f, 170.0f, 26.0f);
    selectors[1].r = juce::Rectangle<float> (oscPanel.getX() + 198.0f, oscPanel.getY() + 118.0f, 170.0f, 26.0f);

    lfoScope = juce::Rectangle<float> (lfoPanel.getX() + 12.0f, lfoPanel.getY() + 34.0f, 356.0f, 100.0f);
    selectors[2].r = juce::Rectangle<float> (lfoPanel.getX() + 12.0f, lfoPanel.getY() + 140.0f, 356.0f, 26.0f);
    for (size_t k = 0; k < 3; ++k)          // über den Reglern Filter, Pitch, Amp (Spalten 1 bis 3 von 4)
        routeLed[k] = { lfoPanel.getX() + 12.0f + ((float) k + 1.5f) * 89.0f, lfoPanel.getY() + 182.0f };

    // Regler nach Gruppen anordnen
    auto place = [] (juce::Rectangle<int> area, int count, std::vector<Knob*> items)
    {
        const int w = area.getWidth() / count;
        for (auto* k : items)
        {
            auto col = area.removeFromLeft (w);
            k->label.setBounds (col.removeFromTop (16));
            k->slider.setBounds (col.reduced (1));
        }
    };
    std::array<std::vector<Knob*>, 5> grp;
    for (size_t i = 0; i < knobs.size(); ++i)
        grp[(size_t) knobInfo[i].group].push_back (&knobs[i]);
    auto& g0 = grp[0]; auto& g1 = grp[1]; auto& g2 = grp[2];

    // untere Leiste: Reihe 1 (Filter, Effekte, Hüllkurve), Reihe 2 (Echo und Reverb)
    place (juce::Rectangle<int> (4, panelTop + 6, getWidth() - 8, 124), (int) g0.size(), g0);
    {
        const int fxY = panelTop + 136;
        const int fxH = getHeight() - fxY - 8;
        const int cw  = (getWidth() - 8) / 9;
        echoPanel = juce::Rectangle<float> (4.0f, (float) fxY, (float) (cw * 5), (float) fxH).reduced (2.0f);
        voidPanel = juce::Rectangle<float> ((float) (4 + cw * 5), (float) fxY, (float) (cw * 4), (float) fxH).reduced (2.0f);
        place (juce::Rectangle<int> (4, fxY + 22, cw * 5, fxH - 22), 5, grp[3]);
        place (juce::Rectangle<int> (4 + cw * 5, fxY + 22, cw * 4, fxH - 22), 4, grp[4]);
    }
    place (juce::Rectangle<int> ((int) oscPanel.getX() + 12, (int) oscPanel.getY() + 172, 356, 116), (int) g1.size(), g1);
    place (juce::Rectangle<int> ((int) lfoPanel.getX() + 12, (int) lfoPanel.getY() + 198, 356, 100), (int) g2.size(), g2);
}

//==============================================================================
float Mkvst04Editor::param (const char* id) const
{
    return proc.apvts.getRawParameterValue (id)->load();
}

void Mkvst04Editor::setChoice (const char* id, int index, int count)
{
    if (auto* p = proc.apvts.getParameter (id))
    {
        p->beginChangeGesture();
        p->setValueNotifyingHost (count > 1 ? (float) index / (float) (count - 1) : 0.0f);
        p->endChangeGesture();
    }
}

juce::Rectangle<float> Mkvst04Editor::keyRect (int key) const
{
    const float h = bldHeight[(size_t) key];
    return { skyX0 + (float) key * keyW, barTop - h, keyW, h + barH };
}

juce::Point<float> Mkvst04Editor::roofPoint (int key) const
{
    const auto r = keyRect (key);
    return { r.getCentreX(), r.getY() - stepH[(size_t) key] - 2.0f };
}

// columnOnly = true: ganze Spalte über dem Haus zählt (damit man beim Ziehen sauber über die
// Skyline gleiten kann, auch über niedrige Häuser).
int Mkvst04Editor::keyAt (juce::Point<float> p, bool columnOnly) const
{
    for (int i = 0; i < numKeys; ++i)
        if (keyRect (i).contains (p))
            return i;
    if (columnOnly && p.y >= barTop - 190.0f && p.y <= barTop + barH
        && p.x >= skyX0 && p.x < skyX0 + skyW)
        return juce::jlimit (0, numKeys - 1, (int) ((p.x - skyX0) / keyW));
    return -1;
}

void Mkvst04Editor::mouseDown (const juce::MouseEvent& e)
{
    // Wellenform-Wähler
    for (auto& s : selectors)
        if (s.r.contains (e.position))
        {
            const int idx = juce::jlimit (0, s.count - 1, (int) ((e.position.x - s.r.getX()) / (s.r.getWidth() / (float) s.count)));
            setChoice (s.id, idx, s.count);
            s.glow = 1.0f;
            activity[(size_t) s.group] = 1.0f;
            return;
        }

    const int key = keyAt (e.position, false);
    if (key >= 0)
    {
        heldKey = key;
        proc.keyboardState.noteOn (1, firstNote + key, 0.8f);
    }
    else if (figureMask.contains (e.position))
        mutate();
}

void Mkvst04Editor::mouseDrag (const juce::MouseEvent& e)
{
    const int key = keyAt (e.position, true);
    if (heldKey >= 0 && key >= 0 && key != heldKey)
    {
        proc.keyboardState.noteOff (1, firstNote + heldKey, 0.0f);
        heldKey = key;
        proc.keyboardState.noteOn (1, firstNote + heldKey, 0.8f);
    }
}

void Mkvst04Editor::mouseUp (const juce::MouseEvent&)
{
    if (heldKey >= 0)
    {
        proc.keyboardState.noteOff (1, firstNote + heldKey, 0.0f);
        heldKey = -1;
    }
}

// "Genom"-Idee: jeder Klick auf eine Figur verschiebt die Klang-Parameter zufällig um bis zu
// +-20 % ihres Wertebereichs und wechselt manchmal die Wellenform. Dazu ein Glitch-Effekt.
void Mkvst04Editor::mutate()
{
    static const char* ids[] = { "dark", "klack", "knatter", "mystery", "clockRate", "clockDepth", "release",
                                 "oscMix", "sub", "lfoRate", "lfoFilter", "echoDark", "revDamp", "revSpook" };
    for (auto* id : ids)
        if (auto* p = proc.apvts.getParameter (id))
        {
            const float v = juce::jlimit (0.0f, 1.0f, p->getValue() + (rng.nextFloat() - 0.5f) * 0.4f);
            p->beginChangeGesture();
            p->setValueNotifyingHost (v);
            p->endChangeGesture();
        }
    if (rng.nextFloat() < 0.35f) setChoice ("osc2Wave", rng.nextInt (4), 4);
    if (rng.nextFloat() < 0.35f) setChoice ("lfoShape", rng.nextInt (5), 5);

    kickL = kickR = kickHead = 1.0f;
    glitch = 1.0f;
    skyFlash = 1.0f;
}

// Sporen steigen wie Glut vom Dach auf und kreisen dabei langsam.
void Mkvst04Editor::spawnSpores (int key, int count)
{
    const auto roof = roofPoint (key);
    const auto col = noteColour (firstNote + key);
    for (int i = 0; i < count && spores.size() < 500; ++i)
    {
        const float life = 1.0f + rng.nextFloat() * 1.2f;
        spores.push_back ({ roof, { (rng.nextFloat() - 0.5f) * 30.0f, -(20.0f + rng.nextFloat() * 50.0f) },
                            life, life, 1.5f + rng.nextFloat() * 2.0f,
                            rng.nextBool() ? col : col.interpolatedWith (juce::Colours::white, 0.6f) });
    }
}

// Funken sprühen aus dem Kopf und fallen mit Schwerkraft nach unten.
void Mkvst04Editor::spawnSparkles (juce::Point<float> at, int count)
{
    for (int i = 0; i < count && sparkles.size() < 200; ++i)
    {
        const float a = rng.nextFloat() * twoPi;
        const float speed = 80.0f + rng.nextFloat() * 160.0f;
        const float life = 0.4f + rng.nextFloat() * 0.4f;
        sparkles.push_back ({ at, { std::cos (a) * speed, std::sin (a) * speed - 60.0f }, life, life });
    }
}

//==============================================================================
void Mkvst04Editor::timerCallback()
{
    const float dt = 1.0f / 60.0f;
    time += dt;

   #if JUCE_WINDOWS
    // Unter Windows zeichnet JUCE 8 standardmäßig mit Direct2D. Dort sehen Verläufe, Clips und das
    // gekachelte Filmkorn anders aus (Kurven gedimmt, Gehirne und Fensterlichter fehlen). Deshalb wie auf
    // Mac und Linux den Software-Renderer nehmen, sobald das Fenster existiert.
    if (! rendererChosen)
        if (auto* peer = getPeer())
        {
            const int sw = peer->getAvailableRenderingEngines().indexOf ("Software Renderer");
            if (sw >= 0 && peer->getCurrentRenderingEngine() != sw)
                peer->setCurrentRenderingEngine (sw);
            rendererChosen = true;
        }
   #endif

    // Takt der DAW: läuft der Transport, folgt die Phase der PPQ-Position (seit dem letzten Audio-Block
    // mit dem Tempo weitergerechnet), sonst läuft sie frei im zuletzt bekannten Tempo (Standard 120 BPM).
    {
        const double bpm = juce::jlimit (20.0, 400.0, proc.hostBpm.load());
        const double since = (juce::Time::getMillisecondCounterHiRes() - proc.hostPpqStamp.load()) / 1000.0;
        if (proc.hostPlaying.load() && since < 0.5)
        {
            const double ppq = proc.hostPpq.load() + since * bpm / 60.0;
            beatPhase = (float) (ppq - std::floor (ppq));
        }
        else
        {
            beatPhase += (float) (dt * bpm / 60.0);
            beatPhase -= std::floor (beatPhase);
        }
        brainPulse = beatPulse (beatPhase);
    }
    level += (proc.level.load() - level) * 0.25f;

    float targetL = 0.0f, targetR = 0.0f;
    for (int i = 0; i < numKeys; ++i)
    {
        const bool on = proc.keyboardState.isNoteOnForChannels (0xffff, firstNote + i);
        const bool leftSide = roofPoint (i).x < sceneCentre.x;
        if (on && ! keyOn[(size_t) i])
        {
            // neue Note: Figuren pochen abwechselnd, Kopf immer, Blitz-Salve aus den Gehirnen, Funken,
            // Sporen auf dem Dach und ein kurzes Aufleuchten des Himmels
            if (i % 2 == 0) kickL = 1.0f; else kickR = 1.0f;
            kickHead = 1.0f;
            skyFlash = 1.0f;
            if (flashes.size() < 40) flashes.push_back ({ i, 1.0f });
            spawnSpores (i, 14);
            spawnSparkles (brainWorldL, 8);
            spawnSparkles (brainWorldR, 8);
        }
        keyOn[(size_t) i] = on;
        keyGlow[(size_t) i] += ((on ? 1.0f : 0.0f) - keyGlow[(size_t) i]) * (on ? 0.5f : 0.08f);
        if (on && rng.nextFloat() < 0.25f)
            spawnSpores (i, 1);
        (leftSide ? targetL : targetR) = juce::jmax (leftSide ? targetL : targetR, keyGlow[(size_t) i]);
    }
    headFlashL += (targetL - headFlashL) * 0.4f;
    headFlashR += (targetR - headFlashR) * 0.4f;

    kickL *= 0.88f;  kickR *= 0.88f;  kickHead *= 0.88f;  glitch *= 0.92f;  skyFlash *= 0.9f;

    for (auto& f : flashes) f.life -= 3.5f * dt;
    flashes.erase (std::remove_if (flashes.begin(), flashes.end(), [] (const Flash& f) { return f.life <= 0.0f; }),
                   flashes.end());

    for (auto& sp : spores)
    {
        sp.p += sp.v * dt;
        sp.v += juce::Point<float> (-sp.v.y, sp.v.x) * (0.4f * dt);    // langsames Kreisen
        sp.v *= 0.985f;
        sp.life -= dt;
    }
    spores.erase (std::remove_if (spores.begin(), spores.end(), [] (const Spore& sp) { return sp.life <= 0.0f; }),
                  spores.end());

    for (auto& sk : sparkles)
    {
        sk.p += sk.v * dt;
        sk.v.y += 260.0f * dt;                                           // Schwerkraft
        sk.life -= dt;
    }
    sparkles.erase (std::remove_if (sparkles.begin(), sparkles.end(), [] (const Sparkle& sk) { return sk.life <= 0.0f; }),
                    sparkles.end());

    for (auto& d : dust)
    {
        d.x += (d.vx + 8.0f * std::sin (time * 0.7f + d.phase)) * dt;      // schaukelndes Treiben
        d.y += d.vy * dt;
        if (d.y < -4.0f) d.y = (float) winH + 4.0f;
        if (d.x < -4.0f) d.x = (float) winW + 4.0f;
        if (d.x > (float) winW + 4.0f) d.x = -4.0f;
    }

    // Visuelle Rückmeldung bei Bedienung: Regler, Wähler und Gruppen-LEDs leuchten und klingen ab.
    // Jeder Regler bekommt pro Frame seine Animationswerte (glow, ripple, spin, t).
    for (size_t i = 0; i < knobs.size(); ++i)
    {
        auto& k = knobs[i];
        if (k.slider.isMouseButtonDown())
        {
            knobGlow[i] = juce::jmax (knobGlow[i], 0.7f);
            activity[(size_t) knobInfo[i].group] = juce::jmax (activity[(size_t) knobInfo[i].group], 0.7f);
        }
        else
            knobGlow[i] *= 0.93f;
        knobRipple[i] *= 0.94f;

        const float v = (float) k.slider.valueToProportionOfLength (k.slider.getValue());
        knobSpin[i] = std::fmod (knobSpin[i] + (0.25f + 1.6f * v + 3.0f * knobGlow[i]) * dt, twoPi);

        auto& props = k.slider.getProperties();
        props.set ("glow", knobGlow[i]);
        props.set ("ripple", knobRipple[i]);
        props.set ("spin", knobSpin[i]);
        props.set ("t", std::fmod (time, 1000.0f));
        k.slider.repaint();
    }
    for (auto& s : selectors) s.glow *= 0.93f;
    for (auto& a : activity) a *= 0.95f;

    repaint();
}

//==============================================================================
void Mkvst04Editor::paint (juce::Graphics& g)
{
    drawBackground (g);
    drawFarCity (g);
    drawFigures (g);
    drawSkyline (g);
    drawFog (g);

    // Filmkorn über der ganzen Szene (wechselt jedes Bild seine Lage, wirkt wie ein Foto)
    g.setTiledImageFill (grain, rng.nextInt (256), rng.nextInt (256), 1.0f);     // Deckkraft 7 % steckt im Bild
    g.fillRect (0, 0, getWidth(), panelTop);
    drawPanels (g);
    drawEffects (g);
    drawTitle (g);

    // untere Leiste (Filter, Effekte, Hüllkurve)
    juce::ColourGradient bar (juce::Colour (0xff182125), 0.0f, (float) panelTop,
                              juce::Colour (0xff0b1012), 0.0f, (float) winH, false);
    g.setGradientFill (bar);
    g.fillRect (0, panelTop, getWidth(), getHeight() - panelTop);
    g.setColour (steelLine);
    g.drawHorizontalLine (panelTop, 0.0f, (float) getWidth());
    g.setColour (juce::Colour (0xff0a0f11));
    g.drawHorizontalLine (panelTop + 1, 0.0f, (float) getWidth());
    g.setColour (juce::Colour (0xff6b777d).withAlpha (0.7f));
    for (int x = 14; x < getWidth(); x += getWidth() / 9)           // Nieten
        g.fillEllipse ((float) x, (float) panelTop + 5.0f, 3.5f, 3.5f);

    drawFxPanels (g);
}

void Mkvst04Editor::drawBackground (juce::Graphics& g)
{
    juce::ColourGradient sky (skyTop, 0.0f, 0.0f, skyLow, 0.0f, (float) winH, false);
    sky.addColour (0.45, skyMid);
    g.setGradientFill (sky);
    g.fillRect (getLocalBounds());

    // große weiche Wolkenbänke, langsam driftend
    static const float cyPos[] = { 160.0f, 260.0f, 120.0f, 340.0f, 210.0f, 300.0f };
    for (int k = 0; k < 6; ++k)
    {
        const float span = (float) winW + 700.0f;
        const float cx = std::fmod ((float) k * 240.0f + time * (3.0f + (float) k), span) - 350.0f;
        const float cy = cyPos[k];
        const float rx = 230.0f + 25.0f * (float) k;
        juce::ColourGradient gr (fogTeal.withAlpha (0.16f), cx, cy, juce::Colours::transparentBlack, cx + rx, cy, true);
        g.setGradientFill (gr);
        g.fillEllipse (cx - rx, cy - rx * 0.45f, rx * 2.0f, rx * 0.9f);
    }

    // Staub und ferne Sterne
    for (const auto& d : dust)
    {
        const float a = 0.05f + 0.2f * (0.5f + 0.5f * std::sin (time * 1.3f + d.phase));
        g.setColour (juce::Colour (0xffa8d8d8).withAlpha (a));
        g.fillEllipse (d.x, d.y, 2.0f, 2.0f);
    }

    juce::ColourGradient vignette (juce::Colours::transparentBlack, (float) winW * 0.5f, 330.0f,
                                   juce::Colours::black.withAlpha (0.7f), 0.0f, 0.0f, true);
    g.setGradientFill (vignette);
    g.fillRect (getLocalBounds());
}

//==============================================================================
// Die zwei Figuren: schwarze Silhouetten (Umriss = Logo-Piktogramm), Nebel zieht von unten hoch.
// In jedem Kopf schimmert ein Gehirn durch, das im Takt der DAW pocht (brainPulse) und bei Noten
// auf seiner Seite aufleuchtet (headFlashL/R). Aus den Gehirnen zucken die Blitze.
void Mkvst04Editor::drawFigures (juce::Graphics& g)
{
    const float energy = juce::jlimit (0.0f, 1.0f, level * 6.0f);
    const float e      = juce::jlimit (0.0f, 1.0f, energy + 0.5f * juce::jmax (kickL, kickR, kickHead));

    // Pochen im Takt + Lautstärke + Anschlag; dazu organisches Schwanken um die Füße
    auto tfFor = [&] (float px, float py, float s, float ang, float rx, float ry)
    {
        return juce::AffineTransform::scale (s, s, px, py)
               .followedBy (juce::AffineTransform::rotation (ang, rx, ry))
               .followedBy (juce::AffineTransform::scale (figureScale))
               .followedBy (juce::AffineTransform::translation (sceneCentre.x, sceneCentre.y));
    };
    const float baseS   = 1.0f + 0.012f * brainPulse + 0.04f * energy;
    const float swayAmp = 0.008f + 0.012f * energy;
    const float swayL   = swayAmp * std::sin (time * 0.9f);
    const float swayR   = swayAmp * std::sin (time * 0.9f + 2.4f);
    const auto tfL = tfFor (-57.0f,    0.0f, baseS + 0.06f * kickL, swayL, -57.0f, 150.0f);
    const auto tfR = tfFor ( 57.0f,    0.0f, baseS + 0.06f * kickR, swayR,  57.0f, 150.0f);
    const auto tfH = tfFor (  0.0f, -117.0f, baseS + 0.08f * kickHead, 0.5f * (swayL + swayR), 0.0f, -80.0f);

    brainWorldL = brainCentreL.transformedBy (tfH);
    brainWorldR = juce::Point<float> (-brainCentreL.x, brainCentreL.y).transformedBy (tfH);

    juce::Path mask;
    mask.addPath (bodyL, tfL);
    mask.addPath (bodyR, tfR);
    mask.addPath (headPath, tfH);
    figureMask = mask;

    // Kanten zuerst: feines Kantenlicht vom Himmel, dazu elektrischer Schein bei Energie. Die Silhouette
    // wird danach darüber gefüllt, so bleibt nur die äußere Hälfte stehen (keine Kanten der Einzelteile).
    g.setColour (fogTeal.brighter (0.4f).withAlpha (0.12f));
    g.strokePath (figureMask, juce::PathStrokeType (9.0f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
    g.setColour (fogTeal.brighter (0.7f).withAlpha (0.7f));
    g.strokePath (figureMask, juce::PathStrokeType (3.0f));
    g.setColour (electric.withAlpha (0.03f + 0.18f * e));
    g.strokePath (figureMask, juce::PathStrokeType (8.0f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
    g.setColour (electric.withAlpha (0.05f + 0.35f * e));
    g.strokePath (figureMask, juce::PathStrokeType (2.0f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));

    // Silhouette: fast schwarz, unten ein Hauch dunkles Teal
    {
        juce::ColourGradient ink (juce::Colour (0xff020304), 0.0f, sceneCentre.y - 230.0f,
                                  juce::Colour (0xff050b0c), 0.0f, sceneCentre.y + 230.0f, false);
        g.setGradientFill (ink);
        g.fillPath (figureMask);
    }

    // Gehirne im Kopf, auf den Kopfumriss begrenzt
    g.saveState();
    g.reduceClipRegion (headPath, tfH);
    drawBrain (g, tfH, false, headFlashL);
    drawBrain (g, tfH, true,  headFlashR);
    g.restoreState();

    // Nebel zieht von unten an den Figuren hoch
    {
        juce::ColourGradient haze (fogTeal.withAlpha (0.0f), 0.0f, sceneCentre.y - 60.0f,
                                   fogTeal.withAlpha (0.42f), 0.0f, sceneCentre.y + 230.0f, false);
        g.setGradientFill (haze);
        g.fillPath (figureMask);
    }

    // Lichtschein der Gehirne fällt durch den Kopf nach außen
    for (int side = 0; side < 2; ++side)
    {
        const auto c = side == 0 ? brainWorldL : brainWorldR;
        const float glow = brainGlow (side == 0 ? headFlashL : headFlashR);
        juce::ColourGradient spill (brainPink.withAlpha (0.10f * glow), c, brainPink.withAlpha (0.0f),
                                    c.translated (95.0f, 0.0f), true);
        g.setGradientFill (spill);
        g.fillEllipse (juce::Rectangle<float> (190.0f, 190.0f).withCentre (c));
    }

    // Glitch beim Mutieren: Umriss in Cyan und Pink seitlich versetzt
    if (glitch > 0.03f)
    {
        const float off = 2.0f + 10.0f * glitch;
        g.setColour (electric.withAlpha (0.4f * glitch));
        g.strokePath (figureMask, juce::PathStrokeType (1.2f), juce::AffineTransform::translation (-off, 0.0f));
        g.setColour (juce::Colour (0xffff5a7a).withAlpha (0.4f * glitch));
        g.strokePath (figureMask, juce::PathStrokeType (1.2f), juce::AffineTransform::translation (off, 0.0f));
    }
}

// Helligkeit eines Gehirns: Grundglimmen, Takt-Puls und Aufleuchten bei Noten (flash 0..1)
float Mkvst04Editor::brainGlow (float flash) const
{
    return juce::jlimit (0.0f, 1.0f, 0.3f + 0.4f * brainPulse + 0.45f * flash);
}

// Gehirn in Frontansicht (zwei Hälften mit Windungen, Schläfenlappen, Hirnstamm).
// Pfade in Figureneinheiten um brainCentreL; der rechte Kopf ist das Spiegelbild (x -> -x).
// Weicher Schein = radialer Verlauf + breite, blasse Striche um den Umriss (JUCE hat keinen Blur).
void Mkvst04Editor::drawBrain (juce::Graphics& g, const juce::AffineTransform& tfHead, bool mirrored, float flash)
{
    const float glow = brainGlow (flash);
    const float grow = 0.86f * (1.0f + 0.035f * brainPulse + 0.03f * flash);
    const auto c = brainCentreL;
    auto tf = juce::AffineTransform::scale (grow, grow, c.x, c.y);
    if (mirrored) tf = tf.followedBy (juce::AffineTransform::scale (-1.0f, 1.0f));
    tf = tf.followedBy (tfHead);

    const auto cw = c.transformedBy (tf);
    juce::ColourGradient halo (brainPink.withAlpha (0.45f * glow), cw, brainPink.withAlpha (0.0f),
                               cw.translated (42.0f * figureScale, 0.0f), true);
    g.setGradientFill (halo);
    g.fillEllipse (juce::Rectangle<float> (84.0f * figureScale, 84.0f * figureScale).withCentre (cw));

    for (float w : { 9.0f, 5.0f })
    {
        g.setColour (brainPink.withAlpha ((w > 6.0f ? 0.07f : 0.13f) * glow));
        g.strokePath (brainShape, juce::PathStrokeType (w, juce::PathStrokeType::curved, juce::PathStrokeType::rounded), tf);
    }
    g.setColour (brainPink.withAlpha (0.14f + 0.32f * glow));
    g.fillPath (brainShape, tf);

    g.saveState();
    g.reduceClipRegion (brainShape, tf);
    g.setColour (brainLight.withAlpha (0.12f * glow));
    g.strokePath (brainFolds, juce::PathStrokeType (3.2f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded), tf);
    g.setColour (brainLight.withAlpha (0.15f + 0.45f * glow));
    g.strokePath (brainFolds, juce::PathStrokeType (1.1f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded), tf);
    g.restoreState();

    g.setColour (juce::Colour (0xff1a0612).withAlpha (0.8f));
    g.strokePath (brainFissure, juce::PathStrokeType (1.4f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded), tf);
}

//==============================================================================
// Skyline im Miniatur-Look: gedämpfte Grau-Beige-Fassaden, warme Fenster, Wassertürme, Gittermasten.
// Gespielte Häuser leuchten in ihrer Tonfarbe, die Nachbarn leuchten etwas mit.
void Mkvst04Editor::drawSkyline (juce::Graphics& g)
{
    const float energy = juce::jlimit (0.0f, 1.0f, level * 6.0f);

    // Dunst hinter den Häusern
    {
        juce::ColourGradient mist (fogTeal.withAlpha (0.0f), 0.0f, barTop - 240.0f,
                                   fogTeal.withAlpha (0.35f + 0.25f * energy), 0.0f, barTop, false);
        g.setGradientFill (mist);
        g.fillRect (juce::Rectangle<float> (0.0f, barTop - 240.0f, (float) winW, 240.0f));
    }

    std::array<float, numKeys> light {};
    for (int i = 0; i < numKeys; ++i)
    {
        float v = keyGlow[(size_t) i];
        for (int d = 1; d <= 2; ++d)
        {
            const float f = d == 1 ? 0.35f : 0.15f;
            if (i - d >= 0)      v = juce::jmax (v, f * keyGlow[(size_t) (i - d)]);
            if (i + d < numKeys) v = juce::jmax (v, f * keyGlow[(size_t) (i + d)]);
        }
        light[(size_t) i] = v;
    }

    for (int i = 0; i < numKeys; ++i)
    {
        const auto r = keyRect (i);
        const int note = firstNote + i;
        const bool black = isBlackKey (note);
        const float lt = light[(size_t) i];
        const float glow = keyGlow[(size_t) i];
        const auto col = noteColour (note);
        const float top = r.getY();
        const juce::Rectangle<float> body (r.getX() + 1.0f, top, r.getWidth() - 2.0f, bldHeight[(size_t) i]);

        const auto c1 = (black ? juce::Colour (0xff33363a) : juce::Colour (0xff4a4d4a)).interpolatedWith (col.darker (0.3f), lt * 0.5f);
        const auto c2 = (black ? juce::Colour (0xff15181b) : juce::Colour (0xff1f2326)).interpolatedWith (col.darker (0.7f), lt * 0.4f);
        const auto edge = juce::Colour (0xff0b0f11).interpolatedWith (col, lt);

        // Dachaufbau: Stufe
        if (hasStep[(size_t) i])
        {
            const juce::Rectangle<float> st (body.getX() + 4.0f, top - stepH[(size_t) i], body.getWidth() - 8.0f, stepH[(size_t) i] + 1.0f);
            g.setColour (c1);
            g.fillRect (st);
            g.setColour (edge);
            g.drawRect (st, 1.0f);
        }

        g.setGradientFill (juce::ColourGradient (c1, body.getX(), top, c2, body.getX(), barTop, false));
        g.fillRect (body);

        // Plastizität: rechte Seite im Schatten, Lichtkante links, Gesims oben, Stockwerkslinien
        g.setGradientFill (juce::ColourGradient (juce::Colours::black.withAlpha (0.0f), body.getCentreX(), 0.0f,
                                                 juce::Colours::black.withAlpha (0.38f), body.getRight(), 0.0f, false));
        g.fillRect (body.withLeft (body.getCentreX()));
        g.setColour (juce::Colours::white.withAlpha (0.10f));
        g.drawLine (body.getX() + 0.5f, top + 2.0f, body.getX() + 0.5f, barTop, 1.0f);
        g.setColour (c1.brighter (0.35f));
        g.fillRect (body.getX() - 1.0f, top, body.getWidth() + 2.0f, 2.5f);
        g.setColour (juce::Colours::black.withAlpha (0.35f));
        g.fillRect (body.getX(), top + 2.5f, body.getWidth(), 1.0f);
        g.setColour (juce::Colours::black.withAlpha (0.10f));
        for (float y = top + 15.5f; y < barTop - 4.0f; y += 9.5f)
            g.drawLine (body.getX() + 1.0f, y, body.getRight() - 1.0f, y, 0.6f);
        g.setColour (edge);
        g.drawRect (body, 1.0f);

        // Fenster: stabil "zufällig" an/aus, einzelne flackern; beim Spielen leuchten alle angeschalteten
        // Dunkle Fenster sind sichtbares Glas, helle haben einen kleinen Lichtschein und einen Rahmen
        juce::Path dark, warm, hot, halo;
        const float spacing = (body.getWidth() - 4.0f) / 4.0f;
        int row = 0;
        for (float y = top + 8.0f; y < barTop - 6.0f; y += 9.5f, ++row)
            for (int c = 0; c < 4; ++c)
            {
                const juce::uint32 h = hash32 ((juce::uint32) (i * 7919 + row * 104729 + c * 1299709));
                bool lit = ((h >> 3) & 7u) < 3u;
                if (std::sin (time * 0.6f + (float) (h & 255u)) > 0.93f) lit = false;
                const juce::Rectangle<float> w (body.getX() + 2.0f + (float) c * spacing + 1.2f, y, 3.2f, 4.6f);
                if (! lit) { dark.addRectangle (w); continue; }
                halo.addRectangle (w.expanded (1.4f));
                if (lt > 0.05f) hot.addRectangle (w); else warm.addRectangle (w);
            }
        g.setColour (juce::Colour (0xff0b1114).withAlpha (0.85f));
        g.fillPath (dark);
        g.setColour (juce::Colour (0xff5d7a80).withAlpha (0.18f));
        g.strokePath (dark, juce::PathStrokeType (0.5f));
        g.setColour ((lt > 0.05f ? col : windowWarm).withAlpha (0.12f));
        g.fillPath (halo);
        g.setColour (windowWarm.withAlpha (0.85f));
        g.fillPath (warm);
        g.setColour (col.interpolatedWith (juce::Colours::white, 0.3f).withAlpha (0.6f + 0.4f * lt));
        g.fillPath (hot);

        const float ay = top - stepH[(size_t) i];
        const float ax = body.getCentreX();

        // Gittermast mit blinkender roter Warnlampe
        if (hasAntenna[(size_t) i])
        {
            g.setColour (juce::Colour (0xff151a1c));
            g.drawLine (ax - 4.5f, ay, ax, ay - 26.0f, 1.2f);
            g.drawLine (ax + 4.5f, ay, ax, ay - 26.0f, 1.2f);
            for (int k = 1; k < 4; ++k)
                g.drawLine (ax - 4.0f + (float) k * 0.9f, ay - (float) k * 6.5f, ax + 4.0f - (float) k * 0.9f, ay - (float) k * 6.5f, 0.8f);
            const float blink = std::pow (juce::jmax (0.0f, std::sin (time * 1.7f + (float) i * 1.3f)), 8.0f);
            g.setColour (ledRed.withAlpha (0.25f + 0.75f * blink));
            g.fillEllipse (ax - 1.8f, ay - 27.8f, 3.6f, 3.6f);
        }

        // Wasserturm auf Stelzen
        if (hasTank[(size_t) i])
        {
            g.setColour (juce::Colour (0xff1c1a18));
            g.drawLine (ax - 4.0f, ay, ax - 3.0f, ay - 6.0f, 1.0f);
            g.drawLine (ax + 4.0f, ay, ax + 3.0f, ay - 6.0f, 1.0f);
            g.setColour (juce::Colour (0xff3a2f26));
            g.fillRect (ax - 5.0f, ay - 15.0f, 10.0f, 9.0f);
            juce::Path roof;
            roof.addTriangle (ax - 6.0f, ay - 15.0f, ax + 6.0f, ay - 15.0f, ax, ay - 20.0f);
            g.fillPath (roof);
        }

        if (glow > 0.05f)        // Lichtschein am Fuß des Hauses
        {
            juce::ColourGradient foot (col.withAlpha (0.35f * glow), r.getCentreX(), barTop,
                                       juce::Colours::transparentBlack, r.getCentreX(), barTop - 60.0f, false);
            g.setGradientFill (foot);
            g.fillRect (juce::Rectangle<float> (r.getX() - 6.0f, barTop - 60.0f, r.getWidth() + 12.0f, 60.0f));
        }
    }

    // Der gerade Balken unter der Skyline mit den Notennamen: dunkler Stahl
    const juce::Rectangle<float> bar (skyX0 - 6.0f, barTop, skyW + 12.0f, barH);
    g.setGradientFill (juce::ColourGradient (juce::Colour (0xff2a3438), 0.0f, barTop, juce::Colour (0xff0e1416), 0.0f, barTop + barH, false));
    g.fillRect (bar);
    for (int i = 0; i < numKeys; ++i)
    {
        const float glow = keyGlow[(size_t) i];
        if (glow > 0.03f)
        {
            g.setColour (noteColour (firstNote + i).withAlpha (0.5f * glow));
            g.fillRect (juce::Rectangle<float> (skyX0 + (float) i * keyW, barTop, keyW, barH));
        }
    }
    g.setColour (steelLine);
    g.drawRect (bar, 1.0f);

    // ein Lichtpunkt wandert über den Balken
    {
        const float x = skyX0 + std::fmod (time * 60.0f, skyW);
        const float x0 = juce::jmax (skyX0 - 6.0f, x - 70.0f);
        juce::ColourGradient trail (teal.withAlpha (0.0f), x - 70.0f, 0.0f, teal.withAlpha (0.7f), x, 0.0f, false);
        g.setGradientFill (trail);
        g.fillRect (juce::Rectangle<float> (x0, barTop - 0.5f, x - x0, 2.0f));
    }

    g.setFont (juce::FontOptions (9.5f));
    for (int i = 0; i < numKeys; ++i)
    {
        const float glow = keyGlow[(size_t) i];
        g.setColour (textCol.withAlpha (0.6f).interpolatedWith (juce::Colours::white, glow));
        g.drawText (juce::MidiMessage::getMidiNoteName (firstNote + i, true, true, 4),
                    juce::Rectangle<float> (skyX0 + (float) i * keyW, barTop + 4.0f, keyW, barH - 8.0f),
                    juce::Justification::centred, false);
    }
}

// Nebelschwaden ziehen durch die Stadt
void Mkvst04Editor::drawFog (juce::Graphics& g)
{
    for (int k = 0; k < 6; ++k)
    {
        const float span = (float) winW + 700.0f;
        const float x = std::fmod ((float) k * 310.0f + time * (6.0f + (float) k * 2.0f), span) - 350.0f;
        const float y = barTop - 40.0f - 30.0f * (float) (k % 3);
        juce::ColourGradient gr (fogTeal.withAlpha (0.14f), x, y, juce::Colours::transparentBlack, x + 220.0f, y, true);
        g.setGradientFill (gr);
        g.fillEllipse (x - 220.0f, y - 60.0f, 440.0f, 120.0f);
    }
}

//==============================================================================
// Bedienfelder: Metallrahmen mit Nieten, Titel und Aktivitäts-LED. Links die Oszillatoren mit zwei
// Scopes und Wellenform-Wählern, rechts der LFO mit Scope, Wähler und Routing-LEDs.
void Mkvst04Editor::drawPanels (juce::Graphics& g)
{
    const float e = juce::jlimit (0.0f, 1.0f, level * 6.0f);

    auto panel = [&] (juce::Rectangle<float> r, const juce::String& title, float act)
    {
        g.setGradientFill (juce::ColourGradient (juce::Colour (0xff1c2528), r.getX(), r.getY(),
                                                 juce::Colour (0xff0e1416), r.getX(), r.getBottom(), false));
        g.fillRoundedRectangle (r, 10.0f);
        g.setColour (steelLine);
        g.drawRoundedRectangle (r, 10.0f, 1.5f);
        g.setColour (juce::Colour (0xff0a0f11));
        g.drawRoundedRectangle (r.reduced (3.0f), 8.0f, 1.0f);
        g.setColour (juce::Colour (0xff6b777d));
        for (float fx : { r.getX() + 8.0f, r.getRight() - 12.0f })
            for (float fy : { r.getY() + 8.0f, r.getBottom() - 12.0f })
                g.fillEllipse (fx, fy, 4.0f, 4.0f);

        g.setFont (juce::FontOptions (11.5f, juce::Font::bold));
        g.setColour (textCol);
        g.drawText (title, juce::Rectangle<float> (r.getX() + 20.0f, r.getY() + 8.0f, 220.0f, 16.0f),
                    juce::Justification::centredLeft, false);

        const juce::Point<float> led (r.getRight() - 28.0f, r.getY() + 16.0f);       // Aktivitäts-LED
        g.setColour (amber.withAlpha (0.12f + 0.4f * act));
        g.fillEllipse (juce::Rectangle<float> (16.0f, 16.0f).withCentre (led));
        g.setColour (amber.withAlpha (0.25f + 0.75f * juce::jlimit (0.0f, 1.0f, act)));
        g.fillEllipse (juce::Rectangle<float> (6.0f, 6.0f).withCentre (led));
    };

    // kleine LED-Pegelreihe am unteren Rand der Felder
    auto ledStrip = [&] (juce::Rectangle<float> r)
    {
        const int n = 24;
        const float w = r.getWidth() / (float) n;
        const int lit = (int) (e * (float) n + 0.5f);
        for (int k = 0; k < n; ++k)
        {
            const auto col = k < 14 ? ledGreen : (k < 20 ? ledYellow : ledRed);
            g.setColour (col.withAlpha (k < lit ? 0.95f : 0.13f));
            g.fillRect (r.getX() + (float) k * w + 1.0f, r.getY(), w - 2.0f, r.getHeight());
        }
    };

    panel (oscPanel, "OSCILLATORS", activity[1]);
    panel (lfoPanel, "LFO  MODULATION", activity[2]);
    ledStrip ({ oscPanel.getX() + 12.0f, oscPanel.getBottom() - 20.0f, 356.0f, 6.0f });
    ledStrip ({ lfoPanel.getX() + 12.0f, lfoPanel.getBottom() - 20.0f, 356.0f, 6.0f });

    // Oszillator-Scopes: Zyklen = 2^(Oktave + 2) (1 bis 16), die Kurve atmet mit der Lautstärke;
    // Osc 2 driftet mit der Verstimmung langsam gegen Osc 1 (Schwebung)
    const float oct1 = std::round (param ("osc1Oct")), oct2 = std::round (param ("osc2Oct"));
    const float detune = param ("osc2Detune");
    const float amp = 0.55f + 0.4f * e;
    drawScope (g, osc1Scope, (int) std::round (param ("osc1Wave")), std::pow (2.0f, oct1 + 2.0f),
               time * 0.6f, amp, juce::jmax (selectors[0].glow, knobGlow[9]), teal);
    drawScope (g, osc2Scope, (int) std::round (param ("osc2Wave")), std::pow (2.0f, oct2 + 2.0f),
               time * 0.6f + detune * 0.01f * time, amp, juce::jmax (selectors[1].glow, knobGlow[10] + knobGlow[11]), amber);

    // LFO-Scope: Zyklen steigen mit der Rate (1 bis 8); ein Punkt läuft mit der echten LFO-Phase mit
    const float rate = param ("lfoRate");
    const int shape = (int) std::round (param ("lfoShape"));
    static const int lfoType[] = { 0, 3, 2, 1, 4 };           // Reihenfolge der LFO-Formen -> waveAt()
    const float fRate = param ("lfoFilter"), fPitch = param ("lfoPitch"), fAmp = param ("lfoAmp");
    const float depth = juce::jmax (fRate, juce::jmax (fPitch, fAmp));
    const float norm = juce::jlimit (0.0f, 1.0f, std::log (rate / 0.05f) / std::log (400.0f));
    const float cycles = std::floor (1.0f + 7.0f * norm);
    const float lfoPhase = time * rate;
    drawScope (g, lfoScope, lfoType[juce::jlimit (0, 4, shape)], cycles, 0.0f, 0.15f + 0.8f * depth,
               juce::jmax (selectors[2].glow, knobGlow[14] + knobGlow[15] + knobGlow[16] + knobGlow[17]), electric);
    {
        const float fx = lfoPhase / cycles - std::floor (lfoPhase / cycles);
        const auto inner = lfoScope.reduced (4.0f, 0.0f);
        const float y = inner.getCentreY() - waveAt (lfoType[juce::jlimit (0, 4, shape)], lfoPhase, 7) * (0.15f + 0.8f * depth) * inner.getHeight() * 0.45f;
        const juce::Point<float> dot (inner.getX() + fx * inner.getWidth(), y);
        g.setColour (juce::Colours::white.withAlpha (0.25f));
        g.fillEllipse (juce::Rectangle<float> (14.0f, 14.0f).withCentre (dot));
        g.setColour (juce::Colours::white);
        g.fillEllipse (juce::Rectangle<float> (5.0f, 5.0f).withCentre (dot));

        // Routing-LEDs: leuchten mit Tiefe und aktuellem LFO-Wert
        const float live = 0.5f + 0.5f * waveAt (lfoType[juce::jlimit (0, 4, shape)], lfoPhase, 7);
        const float depths[] = { fRate, fPitch, fAmp };
        for (size_t k = 0; k < 3; ++k)
        {
            const float a = juce::jlimit (0.0f, 1.0f, 0.1f + depths[k] * (0.3f + 0.7f * live));
            g.setColour (amber.withAlpha (0.25f * a));
            g.fillEllipse (juce::Rectangle<float> (16.0f, 16.0f).withCentre (routeLed[k]));
            g.setColour (amber.withAlpha (a));
            g.fillEllipse (juce::Rectangle<float> (6.0f, 6.0f).withCentre (routeLed[k]));
        }
    }

    for (const auto& s : selectors)
        drawSelector (g, s);
}

// Effekt-Reihe: zwei Felder (Dark Echo, Void Reverb) mit kleinen Live-Anzeigen. Das Echo zeigt eine Balkenreihe,
// jeder Balken um den Feedback-Faktor leiser, ein Lichtimpuls wandert hindurch. Der Reverb zeigt Ringe, die sich
// mit "Size" weiter ausdehnen und mit "Spook" schneller wandern.
void Mkvst04Editor::drawFxPanels (juce::Graphics& g)
{
    auto frame = [&] (juce::Rectangle<float> r, const juce::String& title, float act, juce::Colour c)
    {
        g.setGradientFill (juce::ColourGradient (juce::Colour (0xff1a2326), r.getX(), r.getY(),
                                                 juce::Colour (0xff0c1214), r.getX(), r.getBottom(), false));
        g.fillRoundedRectangle (r, 8.0f);
        g.setColour (steelLine.interpolatedWith (c, juce::jlimit (0.0f, 1.0f, act)));
        g.drawRoundedRectangle (r, 8.0f, 1.3f);
        g.setFont (juce::FontOptions (11.5f, juce::Font::bold));
        g.setColour (textCol);
        g.drawText (title, juce::Rectangle<float> (r.getX() + 14.0f, r.getY() + 4.0f, 120.0f, 16.0f),
                    juce::Justification::centredLeft, false);
        const juce::Point<float> led (r.getRight() - 16.0f, r.getY() + 12.0f);
        g.setColour (c.withAlpha (0.12f + 0.4f * juce::jlimit (0.0f, 1.0f, act)));
        g.fillEllipse (juce::Rectangle<float> (16.0f, 16.0f).withCentre (led));
        g.setColour (c.withAlpha (0.25f + 0.75f * juce::jlimit (0.0f, 1.0f, act)));
        g.fillEllipse (juce::Rectangle<float> (6.0f, 6.0f).withCentre (led));
    };
    frame (echoPanel, "DARK ECHO", activity[3], electric);
    frame (voidPanel, "VOID REVERB", activity[4], teal);

    // Echo-Balken
    {
        const float fb = param ("echoFb"), mixv = param ("echoMix");
        const float tn = juce::jlimit (0.0f, 1.0f, std::log (param ("echoTime") / 20.0f) / std::log (60.0f));
        const float x0 = echoPanel.getX() + 150.0f, y0 = echoPanel.getY() + 19.0f;
        const float spacing = 10.0f + 22.0f * tn;
        const float ph = std::fmod (time * (0.5f + 1.0f * (1.0f - tn)), 1.0f);
        for (int k = 0; k < 12; ++k)
        {
            const float x = x0 + (float) k * spacing;
            if (x > echoPanel.getRight() - 40.0f) break;
            const float hgt = 3.0f + 13.0f * std::pow (fb, (float) k) * (0.3f + 0.7f * mixv);
            const float fl = std::pow (juce::jmax (0.0f, std::sin (twoPi * (ph - (float) k * 0.07f))), 6.0f);
            g.setColour (electric.withAlpha (0.25f + 0.65f * fl));
            g.fillRoundedRectangle (x, y0 - hgt, 4.0f, hgt, 1.5f);
        }
    }

    // Reverb-Ringe
    {
        const float size = param ("revSize"), spook = param ("revSpook"), mixv = param ("revMix");
        const juce::Point<float> c (voidPanel.getX() + 190.0f, voidPanel.getY() + 12.0f);
        for (int j = 0; j < 5; ++j)
        {
            const float t = std::fmod (time * (0.18f + 0.5f * spook) + (float) j * 0.2f, 1.0f);
            g.setColour (teal.withAlpha ((1.0f - t) * (0.2f + 0.6f * mixv)));
            g.drawEllipse (c.x - t * (18.0f + 55.0f * size), c.y - t * 8.0f,
                           2.0f * t * (18.0f + 55.0f * size), 2.0f * t * 8.0f, 1.2f);
        }
    }
}

void Mkvst04Editor::drawScope (juce::Graphics& g, juce::Rectangle<float> r, int type, float cycles, float phase,
                             float amp, float glow, juce::Colour c)
{
    g.setColour (juce::Colour (0xff060b0d));
    g.fillRoundedRectangle (r, 4.0f);
    g.setColour (juce::Colour (0xff1a2a2e));
    for (int k = 1; k < 4; ++k)
        g.drawLine (r.getX() + r.getWidth() * (float) k / 4.0f, r.getY() + 3.0f,
                    r.getX() + r.getWidth() * (float) k / 4.0f, r.getBottom() - 3.0f, 1.0f);
    g.drawLine (r.getX() + 3.0f, r.getCentreY(), r.getRight() - 3.0f, r.getCentreY(), 1.0f);

    const auto p = wavePath (type, r.reduced (4.0f, 0.0f), cycles, phase, amp, 7);
    const juce::PathStrokeType round6 (6.0f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded);
    g.setColour (c.withAlpha (0.08f + 0.22f * juce::jlimit (0.0f, 1.0f, glow)));
    g.strokePath (p, round6);
    g.setColour (c.withAlpha (0.28f));
    g.strokePath (p, juce::PathStrokeType (2.8f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
    g.setColour (c.brighter (0.3f).withAlpha (0.95f));
    g.strokePath (p, juce::PathStrokeType (1.4f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));

    g.setColour (steelLine.interpolatedWith (c, juce::jlimit (0.0f, 1.0f, glow)));
    g.drawRoundedRectangle (r, 4.0f, 1.2f);
}

// Wellenform-Wähler: eine Zelle pro Form mit kleinem Kurvensymbol; die gewählte leuchtet amber,
// ein Klick lässt sie kurz heller aufblitzen.
void Mkvst04Editor::drawSelector (juce::Graphics& g, const Selector& s)
{
    static const int lfoType[] = { 0, 3, 2, 1, 4 };
    const int cur = (int) std::round (param (s.id));
    const float cw = s.r.getWidth() / (float) s.count;

    for (int k = 0; k < s.count; ++k)
    {
        const auto cell = juce::Rectangle<float> (s.r.getX() + (float) k * cw, s.r.getY(), cw, s.r.getHeight()).reduced (2.0f);
        const bool lit = (k == cur);
        g.setColour (juce::Colour (0xff0b1012));
        g.fillRoundedRectangle (cell, 4.0f);
        if (lit)
        {
            g.setColour (amber.withAlpha (0.16f + 0.35f * s.glow));
            g.fillRoundedRectangle (cell, 4.0f);
        }
        g.setColour (lit ? amber : steelLine);
        g.drawRoundedRectangle (cell, 4.0f, lit ? 1.5f : 1.0f);

        const int type = s.kind == 0 ? k : lfoType[k];
        const auto icon = wavePath (type, cell.reduced (7.0f, 4.0f), 1.0f, 0.0f, 0.95f, 3);
        g.setColour (lit ? amber.brighter (0.3f) : textCol.withAlpha (0.55f));
        g.strokePath (icon, juce::PathStrokeType (lit ? 1.8f : 1.2f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
    }
}

//==============================================================================
void Mkvst04Editor::drawEffects (juce::Graphics& g)
{
    // Himmel leuchtet kurz bläulich auf, wenn ein Blitz einschlägt
    if (skyFlash > 0.02f)
    {
        juce::ColourGradient flash (electric.withAlpha (0.16f * skyFlash), sceneCentre.x, sceneCentre.y - 110.0f,
                                    juce::Colours::transparentBlack, 0.0f, 0.0f, true);
        g.setGradientFill (flash);
        g.fillRect (getLocalBounds());
    }

    auto brainFor = [&] (int key) { return roofPoint (key).x < sceneCentre.x ? brainWorldL : brainWorldR; };
    auto originFor = [&] (int key, juce::Point<float> from)
    {
        const auto d = roofPoint (key) - from;
        const float len = juce::jmax (1.0f, std::hypot (d.x, d.y));
        return from + d * (14.0f / len);
    };
    auto pitchOf = [&] (int key) { return (float) key / (float) (numKeys - 1); };
    auto flickerFrame = [&] (float pitch01) { return (int) (time * (18.0f + 14.0f * pitch01)); };

    // Einschlag: Lichtschein auf dem Dach und ein Lichtstrahl nach oben
    auto impact = [&] (juce::Point<float> roof, juce::Colour c, float alpha)
    {
        const float a = juce::jlimit (0.0f, 1.0f, alpha);
        juce::ColourGradient gr (c.withAlpha (0.5f * a), roof, juce::Colours::transparentBlack,
                                 roof.translated (40.0f, 0.0f), true);
        g.setGradientFill (gr);
        g.fillEllipse (juce::Rectangle<float> (80.0f, 80.0f).withCentre (roof));

        juce::ColourGradient beam (c.withAlpha (0.3f * a), 0.0f, roof.y, c.withAlpha (0.0f), 0.0f, roof.y - 230.0f, false);
        g.setGradientFill (beam);
        g.fillRect (juce::Rectangle<float> (roof.x - 4.0f, roof.y - 230.0f, 8.0f, 230.0f));
        beam.multiplyOpacity (0.35f);
        g.setGradientFill (beam);
        g.fillRect (juce::Rectangle<float> (roof.x - 14.0f, roof.y - 230.0f, 28.0f, 230.0f));
    };

    // Dauerblitz, solange eine Note klingt: aus dem Gehirn auf der Seite des Hauses
    for (int i = 0; i < numKeys; ++i)
    {
        const float glow = keyGlow[(size_t) i];
        if (glow < 0.08f) continue;
        const float pitch = pitchOf (i);
        const auto roof = roofPoint (i);
        impact (roof, noteColour (firstNote + i), glow);
        drawBolt (g, originFor (i, brainFor (i)), roof, (juce::int64) i * 7919 + flickerFrame (pitch),
                  glow, pitch, boltColour (pitch, 0.85f));
    }

    // Blitz-Salve bei neuen Noten: aus beiden Gehirnen, klingt schnell ab
    for (const auto& f : flashes)
    {
        const float pitch = pitchOf (f.key);
        const int frame = flickerFrame (pitch);
        const auto roof = roofPoint (f.key);
        const auto col = boltColour (pitch, 0.85f);
        drawBolt (g, originFor (f.key, brainWorldL), roof, (juce::int64) f.key * 131 + frame * 3 + 1, f.life, pitch, col);
        drawBolt (g, originFor (f.key, brainWorldR), roof, (juce::int64) f.key * 197 + frame * 5 + 2, f.life, pitch, col);
    }

    // Lichtbogen zwischen den beiden Gehirnen, sobald es laut wird
    {
        const float e = juce::jlimit (0.0f, 1.0f, level * 9.0f);
        if (e > 0.05f)
            drawBolt (g, brainWorldL + juce::Point<float> (24.0f, 0.0f), brainWorldR - juce::Point<float> (24.0f, 0.0f),
                      90001 + (juce::int64) (time * 22.0f), 0.6f * e, 0.5f, boltColour (0.5f, 0.85f));
    }

    // Funken aus den Gehirnen
    for (const auto& sk : sparkles)
    {
        const float a = juce::jlimit (0.0f, 1.0f, sk.life / sk.maxLife);
        g.setColour (electric.interpolatedWith (juce::Colours::white, 0.7f).withAlpha (a));
        g.drawLine (sk.p.x, sk.p.y, sk.p.x - sk.v.x * 0.03f, sk.p.y - sk.v.y * 0.03f, 1.5f);
        g.setColour (electric.withAlpha (0.25f * a));
        g.fillEllipse (juce::Rectangle<float> (8.0f, 8.0f).withCentre (sk.p));
    }

    // Sporen
    for (const auto& sp : spores)
    {
        const float a = juce::jlimit (0.0f, 1.0f, sp.life / sp.maxLife);
        g.setColour (sp.col.withAlpha (0.18f * a));
        g.fillEllipse (juce::Rectangle<float> (sp.size * 4.0f, sp.size * 4.0f).withCentre (sp.p));
        g.setColour (sp.col.withAlpha (0.9f * a));
        g.fillEllipse (juce::Rectangle<float> (sp.size, sp.size).withCentre (sp.p));
    }
}

void Mkvst04Editor::drawTitle (juce::Graphics& g)
{
    const juce::String title ("M K V S T  0 _ 4");
    g.setFont (juce::FontOptions (30.0f, juce::Font::bold));
    g.setColour (teal.withAlpha (0.3f));
    g.drawText (title, 18, 14, 400, 40, juce::Justification::centredLeft);
    g.drawText (title, 20, 16, 400, 40, juce::Justification::centredLeft);
    g.setColour (juce::Colour (0xffd5e2e4));
    g.drawText (title, 19, 15, 400, 40, juce::Justification::centredLeft);

    g.setFont (juce::FontOptions (12.0f));
    g.setColour (textCol.withAlpha (0.7f));
    g.drawText ("klack  .  knatter  .  mystik", 20, 50, 300, 16, juce::Justification::centredLeft);
}


//==============================================================================
// Filmkorn: 256 x 256 Pixel mit zufälligen Grautönen, einmal erzeugt und gekachelt über die Szene gelegt.
void Mkvst04Editor::buildGrain()
{
    grain = juce::Image (juce::Image::ARGB, 256, 256, true);
    juce::Random r (4242);
    for (int y = 0; y < 256; ++y)
        for (int x = 0; x < 256; ++x)
        {
            const auto v = (juce::uint8) r.nextInt (256);
            grain.setPixelAt (x, y, juce::Colour (v, v, v, (juce::uint8) ((40 + r.nextInt (120)) * 7 / 100)));
        }
}

// Ferne Stadt: eine zweite, dunstige Reihe von Hochhäusern am Horizont hinter den Figuren.
// Sie ist reine Kulisse (nicht spielbar) und gibt dem Bild Tiefe (Luftperspektive).
void Mkvst04Editor::buildFarCity()
{
    juce::Random r (777);
    farBuildings.clear();
    farWindows.clear();
    for (float x = -10.0f; x < (float) winW + 10.0f; )
    {
        const float w = 18.0f + r.nextFloat() * 34.0f;
        const float h = 90.0f + std::pow (r.nextFloat(), 1.5f) * 190.0f;
        const juce::Rectangle<float> b (x, barTop - h, w, h);
        farBuildings.addRectangle (b);
        if (r.nextFloat() < 0.25f)
            farBuildings.addRectangle (b.getCentreX() - 0.6f, b.getY() - 18.0f, 1.2f, 18.0f);   // Antenne
        for (float y = b.getY() + 6.0f; y < barTop - 4.0f; y += 7.0f)
            for (float wx = b.getX() + 3.0f; wx < b.getRight() - 3.0f; wx += 5.0f)
                if (r.nextFloat() < 0.16f)
                    farWindows.addRectangle (wx, y, 1.8f, 2.6f);
        x += w + 2.0f + r.nextFloat() * 10.0f;
    }
}

void Mkvst04Editor::drawFarCity (juce::Graphics& g)
{
    g.setColour (juce::Colour (0xff10262a));
    g.fillPath (farBuildings);
    g.setColour (windowWarm.withAlpha (0.28f));
    g.fillPath (farWindows);

    // Dunst legt sich darüber, unten dichter
    juce::ColourGradient haze (skyMid.withAlpha (0.35f), 0.0f, barTop - 280.0f,
                               fogTeal.withAlpha (0.55f), 0.0f, barTop, false);
    g.setGradientFill (haze);
    g.fillPath (farBuildings);
}
