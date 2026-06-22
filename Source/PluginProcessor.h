/*
  ==============================================================================

    This file contains the basic framework code for a JUCE plugin processor.

  ==============================================================================
*/

#pragma once

#include <JuceHeader.h>
#include "GranularProcessor.h"

//==============================================================================
class Granular_fx_testAudioProcessor  : public juce::AudioProcessor
{
public:
    //==============================================================================
    Granular_fx_testAudioProcessor();
    ~Granular_fx_testAudioProcessor() override;

    //==============================================================================
    void prepareToPlay (double sampleRate, int samplesPerBlock) override;
    void releaseResources() override;

   #ifndef JucePlugin_PreferredChannelConfigurations
    bool isBusesLayoutSupported (const BusesLayout& layouts) const override;
   #endif

    void processBlock (juce::AudioBuffer<float>&, juce::MidiBuffer&) override;

    //==============================================================================
    juce::AudioProcessorEditor* createEditor() override;
    bool hasEditor() const override;

    //==============================================================================
    const juce::String getName() const override;

    bool acceptsMidi() const override;
    bool producesMidi() const override;
    bool isMidiEffect() const override;
    double getTailLengthSeconds() const override;

    //==============================================================================
    int getNumPrograms() override;
    int getCurrentProgram() override;
    void setCurrentProgram (int index) override;
    const juce::String getProgramName (int index) override;
    void changeProgramName (int index, const juce::String& newName) override;

    //==============================================================================
    void getStateInformation (juce::MemoryBlock& destData) override;
    void setStateInformation (const void* data, int sizeInBytes) override;

    // -----------------------------------------------------------------------
    // apvts — the AudioProcessorValueTreeState
    //
    // This object owns all the plugin's parameters. It is declared public so
    // that a GUI editor can attach sliders/knobs to parameters later using
    // SliderAttachment. The actual parameter definitions (names, ranges,
    // defaults) are in createParameterLayout() in PluginProcessor.cpp.
    // -----------------------------------------------------------------------
    juce::AudioProcessorValueTreeState apvts;

private:
    //==============================================================================

    // Defines all parameters. Called once in the constructor to initialise apvts.
    // To change a parameter's range or default value, find this function in
    // PluginProcessor.cpp and edit the values there.
    static juce::AudioProcessorValueTreeState::ParameterLayout createParameterLayout();

    // The granular effect engine.
    GranularProcessor granularProcessor;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (Granular_fx_testAudioProcessor)
};
