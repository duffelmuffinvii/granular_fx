/*
  ==============================================================================

    This file contains the basic framework code for a JUCE plugin processor.

  ==============================================================================
*/

#include "PluginProcessor.h"
#include "PluginEditor.h"

// ============================================================================
// createParameterLayout
//
// Defines every parameter the plugin exposes to the host: its ID string,
// human-readable name, value range, and default value.
//
// THIS IS WHERE TO CHANGE PARAMETER DEFAULTS AND RANGES.
//
// AudioParameterFloat arguments:
//   (ID string, display name, min value, max value, default value)
//
// The ID strings must match exactly what GranularProcessor.h uses when
// calling apvts.getRawParameterValue("...") in prepareToPlay.
// ============================================================================
juce::AudioProcessorValueTreeState::ParameterLayout
Granular_fx_testAudioProcessor::createParameterLayout()
{
    juce::AudioProcessorValueTreeState::ParameterLayout layout;

    // Grain size in milliseconds.
    // Lower = noisy/textural, higher = tonal/smeared.
    layout.add (std::make_unique<juce::AudioParameterFloat> (
        juce::ParameterID("grain_size", 1), "Grain Size (ms)",
        juce::NormalisableRange<float> (10.0f, 500.0f, 0.1f),
        80.0f));

    // How many new grains start per second.
    // Lower = sparse, higher = dense cloud.
    layout.add (std::make_unique<juce::AudioParameterFloat> (
        juce::ParameterID("grain_density", 1), "Grain Density (grains/sec)",
        juce::NormalisableRange<float> (1.0f, 100.0f, 0.1f),
        15.0f));

    // Pitch as discrete octave steps.
    // The index maps to a playback speed ratio in GranularProcessor:
    //   0 = -2 oct (0.25x), 1 = -1 oct (0.5x), 2 = unison (1.0x),
    //   3 = +1 oct (2.0x),  4 = +2 oct (4.0x)
    // Default index 2 = unison.
    layout.add (std::make_unique<juce::AudioParameterChoice> (
        juce::ParameterID ("pitch_ratio", 1), "Pitch",
        juce::StringArray { "-2 oct", "-1 oct", "Unison", "+1 oct", "+2 oct" },
        2));

    // Random variation in grain start position (0 = none, 1 = full buffer scatter).
    layout.add (std::make_unique<juce::AudioParameterFloat> (
        juce::ParameterID("position_scatter", 1), "Position Scatter",
        juce::NormalisableRange<float> (0.0f, 1.0f, 0.01f),
        0.3f));

    // Random variation in grain duration (0 = all grains same length, 1 = wide variation).
    layout.add (std::make_unique<juce::AudioParameterFloat> (
        juce::ParameterID("size_scatter", 1), "Size Scatter",
        juce::NormalisableRange<float> (0.0f, 1.0f, 0.01f),
        0.2f));

    // Random variation in stereo pan per grain (0 = center, 1 = hard L/R).
    layout.add (std::make_unique<juce::AudioParameterFloat> (
        juce::ParameterID("pan_scatter", 1), "Pan Scatter",
        juce::NormalisableRange<float> (0.0f, 1.0f, 0.01f),
        0.4f));

    // Dry/wet mix (0 = fully dry / bypass, 1 = fully wet / granular only).
    layout.add (std::make_unique<juce::AudioParameterFloat> (
        juce::ParameterID("dry_wet", 1), "Dry/Wet",
        juce::NormalisableRange<float> (0.0f, 1.0f, 0.01f),
        0.8f));

    // When on, grains play backwards through the recording buffer.
    layout.add (std::make_unique<juce::AudioParameterBool> (
        juce::ParameterID ("reverse", 1), "Reverse", false));

    // Tempo sync: lock grain density / grain size to a musical subdivision.
    layout.add (std::make_unique<juce::AudioParameterBool> (
        juce::ParameterID ("density_sync", 1), "Density Sync", false));
    layout.add (std::make_unique<juce::AudioParameterBool> (
        juce::ParameterID ("size_sync", 1), "Size Sync", false));

    // Which subdivision to use when sync is on.
    // Index maps to: 0=1/32, 1=1/16, 2=1/8, 3=1/4, 4=1/2, 5=1/1.
    // The fraction is relative to a quarter note (beat).
    layout.add (std::make_unique<juce::AudioParameterChoice> (
        juce::ParameterID ("density_division", 1), "Density Division",
        juce::StringArray { "1/32", "1/16", "1/8", "1/4", "1/2", "1/1" }, 2));  // default 1/8
    layout.add (std::make_unique<juce::AudioParameterChoice> (
        juce::ParameterID ("size_division", 1), "Size Division",
        juce::StringArray { "1/32", "1/16", "1/8", "1/4", "1/2", "1/1" }, 3));  // default 1/4

    // ---- Pitch probability weights ----
    // Each grain independently picks a pitch by weighted random draw.
    // A weight of 0 disables that octave entirely; higher values make it more likely.
    // Weights are normalised at runtime so only their relative values matter.
    // Order: 0=-2oct, 1=-1oct, 2=Unison, 3=+1oct, 4=+2oct.
    // Defaults give a slight bell curve centred on unison.
    const float defaultWeights[5] = { 0.0f, 0.3f, 1.0f, 0.3f, 0.0f };
    const char* weightNames[5]    = { "-2 Oct Weight", "-1 Oct Weight",
                                      "Unison Weight", "+1 Oct Weight", "+2 Oct Weight" };
    const char* weightIds[5]      = { "pitch_weight_0", "pitch_weight_1", "pitch_weight_2",
                                      "pitch_weight_3", "pitch_weight_4" };
    for (int i = 0; i < 5; ++i)
        layout.add (std::make_unique<juce::AudioParameterFloat> (
            juce::ParameterID (weightIds[i], 1), weightNames[i],
            juce::NormalisableRange<float> (0.0f, 1.0f, 0.01f),
            defaultWeights[i]));

    return layout;
}

//==============================================================================
Granular_fx_testAudioProcessor::Granular_fx_testAudioProcessor()
#ifndef JucePlugin_PreferredChannelConfigurations
     : AudioProcessor (BusesProperties()
                     #if ! JucePlugin_IsMidiEffect
                      #if ! JucePlugin_IsSynth
                       .withInput  ("Input",  juce::AudioChannelSet::stereo(), true)
                      #endif
                       .withOutput ("Output", juce::AudioChannelSet::stereo(), true)
                     #endif
                       ),
       // Initialise APVTS after the AudioProcessor base class.
       // Arguments: the processor itself, an UndoManager (nullptr = none),
       // a name for the state tree, and the parameter layout we just defined.
       apvts (*this, nullptr, "Parameters", createParameterLayout())
#endif
{
}

Granular_fx_testAudioProcessor::~Granular_fx_testAudioProcessor()
{
}

//==============================================================================
const juce::String Granular_fx_testAudioProcessor::getName() const
{
    return JucePlugin_Name;
}

bool Granular_fx_testAudioProcessor::acceptsMidi() const
{
   #if JucePlugin_WantsMidiInput
    return true;
   #else
    return false;
   #endif
}

bool Granular_fx_testAudioProcessor::producesMidi() const
{
   #if JucePlugin_ProducesMidiOutput
    return true;
   #else
    return false;
   #endif
}

bool Granular_fx_testAudioProcessor::isMidiEffect() const
{
   #if JucePlugin_IsMidiEffect
    return true;
   #else
    return false;
   #endif
}

double Granular_fx_testAudioProcessor::getTailLengthSeconds() const
{
    return 0.0;
}

int Granular_fx_testAudioProcessor::getNumPrograms()
{
    return 1;   // NB: some hosts don't cope very well if you tell them there are 0 programs,
                // so this should be at least 1, even if you're not really implementing programs.
}

int Granular_fx_testAudioProcessor::getCurrentProgram()
{
    return 0;
}

void Granular_fx_testAudioProcessor::setCurrentProgram (int index)
{
}

const juce::String Granular_fx_testAudioProcessor::getProgramName (int index)
{
    return {};
}

void Granular_fx_testAudioProcessor::changeProgramName (int index, const juce::String& newName)
{
}

//==============================================================================
void Granular_fx_testAudioProcessor::prepareToPlay (double sampleRate, int samplesPerBlock)
{
    // Pass the APVTS reference into the granular processor so it can cache
    // parameter pointers for lock-free access on the audio thread.
    granularProcessor.prepareToPlay (sampleRate, samplesPerBlock, apvts);
}

void Granular_fx_testAudioProcessor::releaseResources()
{
}

#ifndef JucePlugin_PreferredChannelConfigurations
bool Granular_fx_testAudioProcessor::isBusesLayoutSupported (const BusesLayout& layouts) const
{
  #if JucePlugin_IsMidiEffect
    juce::ignoreUnused (layouts);
    return true;
  #else
    if (layouts.getMainOutputChannelSet() != juce::AudioChannelSet::mono()
     && layouts.getMainOutputChannelSet() != juce::AudioChannelSet::stereo())
        return false;

   #if ! JucePlugin_IsSynth
    if (layouts.getMainOutputChannelSet() != layouts.getMainInputChannelSet())
        return false;
   #endif

    return true;
  #endif
}
#endif

void Granular_fx_testAudioProcessor::processBlock (juce::AudioBuffer<float>& buffer,
                                                    juce::MidiBuffer& midiMessages)
{
    juce::ScopedNoDenormals noDenormals;
    auto totalNumInputChannels  = getTotalNumInputChannels();
    auto totalNumOutputChannels = getTotalNumOutputChannels();

    for (auto i = totalNumInputChannels; i < totalNumOutputChannels; ++i)
        buffer.clear (i, 0, buffer.getNumSamples());

    // Get host tempo for tempo-sync features. Falls back to 120 BPM if unavailable.
    double bpm = 120.0;
    if (auto* ph = getPlayHead())
        if (auto pos = ph->getPosition())
            if (pos->getBpm().hasValue())
                bpm = *pos->getBpm();

    granularProcessor.processBlock (buffer, totalNumInputChannels, bpm);
}

//==============================================================================
bool Granular_fx_testAudioProcessor::hasEditor() const
{
    return true;
}

juce::AudioProcessorEditor* Granular_fx_testAudioProcessor::createEditor()
{
    return new Granular_fx_testAudioProcessorEditor(*this);
}

//==============================================================================
void Granular_fx_testAudioProcessor::getStateInformation (juce::MemoryBlock& destData)
{
    // Serialise the entire APVTS parameter state to XML and store it in destData.
    // The host calls this when saving a project or preset.
    auto state = apvts.copyState();
    std::unique_ptr<juce::XmlElement> xml (state.createXml());
    copyXmlToBinary (*xml, destData);
}

void Granular_fx_testAudioProcessor::setStateInformation (const void* data, int sizeInBytes)
{
    // Restore parameter state from data previously saved by getStateInformation.
    // The host calls this when loading a project or preset.
    std::unique_ptr<juce::XmlElement> xml (getXmlFromBinary (data, sizeInBytes));
    if (xml != nullptr && xml->hasTagName (apvts.state.getType()))
        apvts.replaceState (juce::ValueTree::fromXml (*xml));
}

//==============================================================================
juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter()
{
    return new Granular_fx_testAudioProcessor();
}
