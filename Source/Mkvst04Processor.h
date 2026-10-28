#pragma once
#include <juce_audio_utils/juce_audio_utils.h>
#include "Mkvst04Voice.h"
#include "Mkvst04Fx.h"

class Mkvst04Processor : public juce::AudioProcessor
{
public:
    Mkvst04Processor();

    void prepareToPlay (double sampleRate, int samplesPerBlock) override;
    void releaseResources() override {}
    bool isBusesLayoutSupported (const BusesLayout& layouts) const override;
    void processBlock (juce::AudioBuffer<float>&, juce::MidiBuffer&) override;

    juce::AudioProcessorEditor* createEditor() override;
    bool hasEditor() const override { return true; }

    const juce::String getName() const override { return "mkvst0_4"; }
    bool acceptsMidi() const override  { return true; }
    bool producesMidi() const override { return false; }
    bool isMidiEffect() const override { return false; }
    double getTailLengthSeconds() const override { return 10.0; }

    int getNumPrograms() override { return 1; }
    int getCurrentProgram() override { return 0; }
    void setCurrentProgram (int) override {}
    const juce::String getProgramName (int) override { return {}; }
    void changeProgramName (int, const juce::String&) override {}

    void getStateInformation (juce::MemoryBlock& destData) override;
    void setStateInformation (const void* data, int sizeInBytes) override;

    juce::AudioProcessorValueTreeState apvts;
    juce::MidiKeyboardState keyboardState;     // Klicks im Notenring + MIDI von außen
    std::atomic<float> level { 0.0f };         // Lautstärke fürs Pochen der Figuren

    // Takt vom Host (für die pochenden Gehirne): Tempo, PPQ-Position zum Zeitpunkt hostPpqStamp (ms)
    std::atomic<double> hostBpm { 120.0 }, hostPpq { 0.0 }, hostPpqStamp { 0.0 };
    std::atomic<bool> hostPlaying { false };

private:
    static juce::AudioProcessorValueTreeState::ParameterLayout createLayout();

    juce::Synthesiser synth;
    DarkEcho echo;
    VoidReverb reverb;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (Mkvst04Processor)
};
