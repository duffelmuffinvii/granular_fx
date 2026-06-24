/*
  ==============================================================================

    This file contains the basic framework code for a JUCE plugin editor.

  ==============================================================================
*/

#include "PluginProcessor.h"
#include "PluginEditor.h"

static constexpr int DIAL_SIZE     = 80;
static constexpr int LABEL_HEIGHT  = 20;
static constexpr int PADDING       = 16;
static constexpr int NUM_CONTROLS  = 6;  // dials only; pitch hidden while weights are active
static constexpr int WEIGHT_W      = 34;  // width of each pitch weight slider
static constexpr int WEIGHT_GAP    = 4;   // gap between weight sliders

//==============================================================================
Granular_fx_testAudioProcessorEditor::Granular_fx_testAudioProcessorEditor (Granular_fx_testAudioProcessor& p)
    : AudioProcessorEditor (&p), audioProcessor (p),
      grainSizeAttachment       (p.apvts, "grain_size",        grainSizeSlider),
      grainDensityAttachment    (p.apvts, "grain_density",     grainDensitySlider),
      positionScatterAttachment (p.apvts, "position_scatter",  positionScatterSlider),
      sizeScatterAttachment     (p.apvts, "size_scatter",      sizeScatterSlider),
      panScatterAttachment      (p.apvts, "pan_scatter",       panScatterSlider),
      dryWetAttachment          (p.apvts, "dry_wet",           dryWetSlider),
      reverseAttachment         (p.apvts, "reverse",           reverseButton),
      seqLengthAttachment       (p.apvts, "seq_length",        seqLengthSlider),
      densitySyncAttachment     (p.apvts, "density_sync",      densitySyncButton),
      sizeSyncAttachment        (p.apvts, "size_sync",         sizeSyncButton),
      densityDivisionAttachment (p.apvts, "density_division",  densityDivisionBox),
      sizeDivisionAttachment    (p.apvts, "size_division",     sizeDivisionBox)
{
    auto setupDial = [this] (juce::Slider& slider, juce::Label& label, const juce::String& labelText)
    {
        slider.setSliderStyle (juce::Slider::RotaryVerticalDrag);
        slider.setTextBoxStyle (juce::Slider::TextBoxBelow, false, DIAL_SIZE, 16);
        slider.setColour (juce::Slider::textBoxTextColourId,       juce::Colour (0xffdddddd));
        slider.setColour (juce::Slider::textBoxBackgroundColourId, juce::Colour (0xff1c1c1c));
        addAndMakeVisible (slider);

        label.setText (labelText, juce::dontSendNotification);
        label.setJustificationType (juce::Justification::centred);
        label.setFont (juce::FontOptions (11.0f));
        addAndMakeVisible (label);
    };

    // Apply the custom look and feel to this editor and all child components.
    setLookAndFeel (&lookAndFeel);

    setupDial (grainSizeSlider,       grainSizeLabel,       "Grain Size");
    setupDial (grainDensitySlider,    grainDensityLabel,    "Density");
    setupDial (positionScatterSlider, positionScatterLabel, "Pos Scatter");
    setupDial (sizeScatterSlider,     sizeScatterLabel,     "Size Scatter");
    setupDial (panScatterSlider,      panScatterLabel,      "Pan Scatter");
    setupDial (dryWetSlider,          dryWetLabel,          "Dry / Wet");

    reverseButton.setButtonText ("");
    addAndMakeVisible (reverseButton);

    reverseLabel.setText ("Reverse", juce::dontSendNotification);
    reverseLabel.setJustificationType (juce::Justification::centred);
    reverseLabel.setFont (juce::FontOptions (11.0f));
    addAndMakeVisible (reverseLabel);

    // Sync toggles — labelled via setButtonText so no separate label is needed.
    densitySyncButton.setButtonText ("BPM Sync");
    addAndMakeVisible (densitySyncButton);

    sizeSyncButton.setButtonText ("BPM Sync");
    addAndMakeVisible (sizeSyncButton);

    // Populate division combo boxes with the subdivision choices.
    // ComboBoxAttachment only syncs selection — it does not add items.
    auto setupDivisionBox = [&] (juce::ComboBox& box, const juce::String& paramId)
    {
        box.addItem ("1/32", 1);
        box.addItem ("1/16", 2);
        box.addItem ("1/8",  3);
        box.addItem ("1/4",  4);
        box.addItem ("1/2",  5);
        box.addItem ("1/1",  6);
        // Sync initial selection to the current parameter value.
        int idx = static_cast<int> (*p.apvts.getRawParameterValue (paramId));
        box.setSelectedId (idx + 1, juce::dontSendNotification);
    };
    setupDivisionBox (densityDivisionBox, "density_division");
    setupDivisionBox (sizeDivisionBox,    "size_division");

    addAndMakeVisible (densityDivisionBox);
    addAndMakeVisible (sizeDivisionBox);

    // Pitch sequencer section
    seqHeaderLabel.setText ("PITCH SEQ", juce::dontSendNotification);
    seqHeaderLabel.setJustificationType (juce::Justification::left);
    seqHeaderLabel.setFont (juce::FontOptions (10.0f));
    seqHeaderLabel.setColour (juce::Label::textColourId, juce::Colour (0xff888888));
    addAndMakeVisible (seqHeaderLabel);

    // Length dial — same compact style as other dials but narrower.
    seqLengthSlider.setSliderStyle (juce::Slider::LinearVertical);
    seqLengthSlider.setTextBoxStyle (juce::Slider::TextBoxBelow, true, WEIGHT_W, 14);
    seqLengthSlider.setColour (juce::Slider::textBoxTextColourId,       juce::Colour (0xffdddddd));
    seqLengthSlider.setColour (juce::Slider::textBoxBackgroundColourId, juce::Colour (0xff1c1c1c));
    addAndMakeVisible (seqLengthSlider);

    seqLengthLabel.setText ("LEN", juce::dontSendNotification);
    seqLengthLabel.setJustificationType (juce::Justification::centred);
    seqLengthLabel.setFont (juce::FontOptions (11.0f));
    addAndMakeVisible (seqLengthLabel);

    static const char* pitchNames[] = { "-2", "-1", "0", "+1", "+2" };

    for (int i = 0; i < 8; ++i)
    {
        seqStepSliders[i].setSliderStyle (juce::Slider::LinearVertical);
        seqStepSliders[i].setTextBoxStyle (juce::Slider::TextBoxBelow, true, WEIGHT_W, 14);
        seqStepSliders[i].setColour (juce::Slider::textBoxTextColourId,       juce::Colour (0xffdddddd));
        seqStepSliders[i].setColour (juce::Slider::textBoxBackgroundColourId, juce::Colour (0xff1c1c1c));
        seqStepSliders[i].textFromValueFunction = [] (double v) -> juce::String {
            return pitchNames[juce::jlimit (0, 4, (int) std::round (v))];
        };
        addAndMakeVisible (seqStepSliders[i]);

        seqStepLabels[i].setText (juce::String (i + 1), juce::dontSendNotification);
        seqStepLabels[i].setJustificationType (juce::Justification::centred);
        seqStepLabels[i].setFont (juce::FontOptions (11.0f));
        addAndMakeVisible (seqStepLabels[i]);

        seqStepAttachments[i] = std::make_unique<SliderAttachment> (
            p.apvts, "seq_step_" + juce::String (i), seqStepSliders[i]);
    }

    // Cache sync param pointers so timerCallback can read them without a hash lookup.
    densitySyncParam = p.apvts.getRawParameterValue ("density_sync");
    sizeSyncParam    = p.apvts.getRawParameterValue ("size_sync");

    // Poll every ~100 ms to grey out / restore dials as sync toggles change.
    startTimerHz (10);

    setSize (PADDING + (NUM_CONTROLS + 1) * (DIAL_SIZE + PADDING),
             PADDING * 4 + LABEL_HEIGHT * 3 + (DIAL_SIZE + 16) + (DIAL_SIZE + 14) + 44);
}

Granular_fx_testAudioProcessorEditor::~Granular_fx_testAudioProcessorEditor()
{
    stopTimer();
    // Must clear the LookAndFeel before it is destroyed, otherwise child
    // components may try to use a dangling pointer during teardown.
    setLookAndFeel (nullptr);
}

void Granular_fx_testAudioProcessorEditor::timerCallback()
{
    // Enable/disable dials and combo boxes based on sync toggle state.
    // When sync is on, the dial is greyed out and the division combo is active.
    const bool densitySync = densitySyncParam && densitySyncParam->load() >= 0.5f;
    const bool sizeSync    = sizeSyncParam    && sizeSyncParam->load()    >= 0.5f;

    grainDensitySlider.setEnabled  (!densitySync);
    densityDivisionBox.setEnabled  (densitySync);

    grainSizeSlider.setEnabled     (!sizeSync);
    sizeDivisionBox.setEnabled     (sizeSync);
}

//==============================================================================
void Granular_fx_testAudioProcessorEditor::paint (juce::Graphics& g)
{
    g.fillAll (getLookAndFeel().findColour (juce::ResizableWindow::backgroundColourId));
}

void Granular_fx_testAudioProcessorEditor::resized()
{
    int x = PADDING;
    const int y = PADDING;

    // y-position just below the dial row: top padding + label + dial + textbox
    const int syncRowY  = y + LABEL_HEIGHT + DIAL_SIZE + 16 + PADDING;
    const int toggleH   = 20;
    const int comboH    = 20;
    const int comboGap  = 4;

    auto placeControl = [&] (juce::Slider& slider, juce::Label& label)
    {
        label.setBounds  (x, y, DIAL_SIZE, LABEL_HEIGHT);
        slider.setBounds (x, y + LABEL_HEIGHT, DIAL_SIZE, DIAL_SIZE + 16);
        x += DIAL_SIZE + PADDING;
    };

    // Grain size — remembering its x before placeControl advances it.
    const int sizeX = x;
    placeControl (grainSizeSlider, grainSizeLabel);

    const int densityX = x;
    placeControl (grainDensitySlider, grainDensityLabel);

    // Pitch slider is hidden while the sequencer is active — skip its slot.
    placeControl (positionScatterSlider, positionScatterLabel);
    placeControl (sizeScatterSlider,     sizeScatterLabel);
    placeControl (panScatterSlider,      panScatterLabel);
    placeControl (dryWetSlider,          dryWetLabel);

    // Reverse toggle — label above, button centred in the slot below.
    reverseLabel.setBounds  (x, y, DIAL_SIZE, LABEL_HEIGHT);
    reverseButton.setBounds (x + DIAL_SIZE / 4, y + LABEL_HEIGHT + (DIAL_SIZE / 2) - 10,
                             DIAL_SIZE / 2, 20);

    // Sync controls: toggle and combo each span the full dial-slot width.
    sizeSyncButton.setBounds    (sizeX,    syncRowY,                      DIAL_SIZE, toggleH);
    sizeDivisionBox.setBounds   (sizeX,    syncRowY + toggleH + comboGap,  DIAL_SIZE, comboH);

    densitySyncButton.setBounds (densityX, syncRowY,                      DIAL_SIZE, toggleH);
    densityDivisionBox.setBounds(densityX, syncRowY + toggleH + comboGap,  DIAL_SIZE, comboH);

    // Pitch sequencer — below the sync controls.
    const int seqRowY = syncRowY + toggleH + comboGap + comboH + PADDING;

    seqHeaderLabel.setBounds (PADDING, seqRowY, 80, LABEL_HEIGHT);

    // Length slider sits to the right of the header, same height as step sliders.
    const int lenX = PADDING + 80 + WEIGHT_GAP;
    seqLengthLabel.setBounds  (lenX, seqRowY,                              WEIGHT_W, LABEL_HEIGHT);
    seqLengthSlider.setBounds (lenX, seqRowY + LABEL_HEIGHT + LABEL_HEIGHT, WEIGHT_W, DIAL_SIZE + 14);

    // Step sliders start after a small gap following the length slider.
    int sx = lenX + WEIGHT_W + WEIGHT_GAP * 3;
    for (int i = 0; i < 8; ++i)
    {
        seqStepLabels[i].setBounds  (sx, seqRowY + LABEL_HEIGHT,               WEIGHT_W, LABEL_HEIGHT);
        seqStepSliders[i].setBounds (sx, seqRowY + LABEL_HEIGHT + LABEL_HEIGHT, WEIGHT_W, DIAL_SIZE + 14);
        sx += WEIGHT_W + WEIGHT_GAP;
    }
}
