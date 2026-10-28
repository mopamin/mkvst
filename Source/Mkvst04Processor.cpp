#include "Mkvst04Processor.h"
#include "Mkvst04Editor.h"

Mkvst04Processor::Mkvst04Processor()
    : AudioProcessor (BusesProperties().withOutput ("Output", juce::AudioChannelSet::stereo(), true)),
      apvts (*this, nullptr, "PARAMS", createLayout())
{
    synth.addSound (new Mkvst04Sound());
    for (int i = 0; i < 8; ++i)
        synth.addVoice (new Mkvst04Voice (apvts));
}

juce::AudioProcessorValueTreeState::ParameterLayout Mkvst04Processor::createLayout()
{
    using namespace juce;
    using Range = NormalisableRange<float>;
    AudioProcessorValueTreeState::ParameterLayout layout;

    auto add = [&] (const char* id, const char* name, Range r, float def)
    {
        layout.add (std::make_unique<AudioParameterFloat> (ParameterID { id, 1 }, name, r, def));
    };
    auto addChoice = [&] (const char* id, const char* name, StringArray options, int def)
    {
        layout.add (std::make_unique<AudioParameterChoice> (ParameterID { id, 1 }, name, options, def));
    };
    auto addInt = [&] (const char* id, const char* name, int lo, int hi, int def)
    {
        layout.add (std::make_unique<AudioParameterInt> (ParameterID { id, 1 }, name, lo, hi, def));
    };

    // Filter, Effekte, Hüllkurve
    add ("dark",       "Dunkel",    Range (0.0f, 1.0f),              0.6f);
    add ("klack",      "Klack",     Range (0.0f, 1.0f),              0.5f);
    add ("knatter",    "Knatter",   Range (0.0f, 1.0f),              0.25f);
    add ("mystery",    "Mystik",    Range (0.0f, 1.0f),              0.5f);
    add ("clockRate",  "Uhr-Takt",  Range (0.5f, 16.0f, 0.0f, 0.5f), 3.0f);
    add ("clockDepth", "Uhr-Tiefe", Range (0.0f, 1.0f),              0.4f);
    add ("attack",     "Attack",    Range (0.002f, 2.0f, 0.0f, 0.4f), 0.02f);
    add ("release",    "Release",   Range (0.05f, 4.0f, 0.0f, 0.4f),  0.8f);
    add ("gain",       "Gain",      Range (0.0f, 1.0f),              0.7f);

    // Oszillatoren: Wellenform 0 Sinus, 1 Säge, 2 Rechteck, 3 Dreieck
    addChoice ("osc1Wave", "Osc 1 Wave", { "Sine", "Saw", "Square", "Tri" }, 1);
    addChoice ("osc2Wave", "Osc 2 Wave", { "Sine", "Saw", "Square", "Tri" }, 1);
    addInt    ("osc1Oct",  "Osc 1 Oktave", -2, 2, 0);
    addInt    ("osc2Oct",  "Osc 2 Oktave", -2, 2, 0);
    add       ("osc2Detune", "Osc 2 Detune", Range (-50.0f, 50.0f, 1.0f), 7.0f);   // Cent
    add       ("oscMix",   "Osc Mix",    Range (0.0f, 1.0f),              0.43f);
    add       ("sub",      "Sub",        Range (0.0f, 1.0f),              0.5f);

    // LFO: Form 0 Sinus, 1 Dreieck, 2 Rechteck, 3 Säge, 4 Sample & Hold
    addChoice ("lfoShape", "LFO Shape",  { "Sine", "Tri", "Square", "Saw", "S&H" }, 0);
    add       ("lfoRate",   "LFO Rate",   Range (0.05f, 20.0f, 0.0f, 0.4f), 0.4f);
    add       ("lfoFilter", "LFO Filter", Range (0.0f, 1.0f),              0.3f);
    add       ("lfoPitch",  "LFO Pitch",  Range (0.0f, 1.0f),              0.0f);
    add       ("lfoAmp",    "LFO Amp",    Range (0.0f, 1.0f),              0.0f);

    // Eigene Effekte: DarkEcho (Ping-Pong mit dunkler Rückkopplung) und VoidReverb (FDN-Hall)
    add       ("echoTime",  "Echo Time",  Range (20.0f, 1200.0f, 0.0f, 0.4f), 380.0f);   // ms
    add       ("echoFb",    "Echo Feedback", Range (0.0f, 0.92f),            0.45f);
    add       ("echoDark",  "Echo Dark",  Range (0.0f, 1.0f),              0.5f);
    add       ("echoDrift", "Echo Drift", Range (0.0f, 1.0f),              0.35f);
    add       ("echoMix",   "Echo Mix",   Range (0.0f, 1.0f),              0.2f);
    add       ("revSize",   "Void Size",  Range (0.0f, 1.0f),              0.55f);
    add       ("revDamp",   "Void Damp",  Range (0.0f, 1.0f),              0.5f);
    add       ("revSpook",  "Void Spook", Range (0.0f, 1.0f),              0.35f);
    add       ("revMix",    "Void Mix",   Range (0.0f, 1.0f),              0.25f);
    return layout;
}

void Mkvst04Processor::prepareToPlay (double sampleRate, int samplesPerBlock)
{
    synth.setCurrentPlaybackSampleRate (sampleRate);
    for (int i = 0; i < synth.getNumVoices(); ++i)
        if (auto* v = dynamic_cast<Mkvst04Voice*> (synth.getVoice (i)))
            v->prepare (sampleRate, samplesPerBlock);

    echo.prepare (sampleRate);
    reverb.prepare (sampleRate);
}

bool Mkvst04Processor::isBusesLayoutSupported (const BusesLayout& layouts) const
{
    return layouts.getMainOutputChannelSet() == juce::AudioChannelSet::stereo();
}

void Mkvst04Processor::processBlock (juce::AudioBuffer<float>& buffer, juce::MidiBuffer& midi)
{
    juce::ScopedNoDenormals noDenormals;
    buffer.clear();

    // Tempo und Position vom Host merken; ohne Angabe bleibt das letzte Tempo (Standard 120 BPM)
    bool playing = false;
    if (auto* ph = getPlayHead())
        if (const auto pos = ph->getPosition())
        {
            if (const auto bpm = pos->getBpm())
                hostBpm.store (*bpm);
            if (const auto ppq = pos->getPpqPosition(); ppq && pos->getIsPlaying())
            {
                hostPpq.store (*ppq);
                hostPpqStamp.store (juce::Time::getMillisecondCounterHiRes());
                playing = true;
            }
        }
    hostPlaying.store (playing);

    // Klicks im Notenring in MIDI umwandeln und eingehendes MIDI für die Anzeige merken
    keyboardState.processNextMidiBuffer (midi, 0, buffer.getNumSamples(), true);

    synth.renderNextBlock (buffer, midi, 0, buffer.getNumSamples());

    // Effekte: erst Echo, dann Hall (so bekommen auch die Echos Raum)
    if (buffer.getNumChannels() >= 2)
    {
        auto p = [this] (const char* id) { return apvts.getRawParameterValue (id)->load(); };
        echo.setParams (p ("echoTime"), p ("echoFb"), p ("echoDark"), p ("echoDrift"), p ("echoMix"));
        reverb.setParams (p ("revSize"), p ("revDamp"), p ("revSpook"), p ("revMix"));
        auto* l = buffer.getWritePointer (0);
        auto* r = buffer.getWritePointer (1);
        for (int i = 0; i < buffer.getNumSamples(); ++i)
        {
            echo.process (l[i], r[i]);
            reverb.process (l[i], r[i]);
        }
    }

    buffer.applyGain (apvts.getRawParameterValue ("gain")->load());

    const float rms = buffer.getRMSLevel (0, 0, buffer.getNumSamples());
    level.store (level.load() * 0.7f + rms * 0.3f);
}

juce::AudioProcessorEditor* Mkvst04Processor::createEditor()
{
    return new Mkvst04Editor (*this);
}

void Mkvst04Processor::getStateInformation (juce::MemoryBlock& destData)
{
    auto state = apvts.copyState();
    if (auto xml = state.createXml())
        copyXmlToBinary (*xml, destData);
}

void Mkvst04Processor::setStateInformation (const void* data, int sizeInBytes)
{
    if (auto xml = getXmlFromBinary (data, sizeInBytes))
        if (xml->hasTagName (apvts.state.getType()))
            apvts.replaceState (juce::ValueTree::fromXml (*xml));
}

juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter()
{
    return new Mkvst04Processor();
}
