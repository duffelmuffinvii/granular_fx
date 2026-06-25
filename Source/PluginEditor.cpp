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
static constexpr int NUM_CONTROLS  = 5;  // dials in the main row (dry/wet moved to left column)
static constexpr int DW_W          = 36; // width of the dry/wet slider column
static constexpr int WEIGHT_W      = 34;  // width of each pitch weight slider
static constexpr int WEIGHT_GAP    = 4;   // gap between weight sliders

// ============================================================================
// PitchSeqEditor implementation
// ============================================================================

// Snap levels (ratio): -2, -1, 0, +1, +2 octaves
static constexpr float kSnapRatios[] = { 0.25f, 0.5f, 1.0f, 2.0f, 4.0f };
static constexpr float kSnapLabels[] = { -2.0f, -1.0f, 0.0f, 1.0f, 2.0f };

// Coordinate transforms — log2 pitch space (top=+2oct, bottom=-2oct)
float PitchSeqEditor::xFromBeat  (float beat)  const { return beat * (float) getWidth() / juce::jmax (0.001f, pattern.patternBeats); }
float PitchSeqEditor::beatFromX  (float x)     const { return x * pattern.patternBeats / juce::jmax (1.0f, (float) getWidth()); }
float PitchSeqEditor::yFromRatio (float ratio) const
{
    float log = std::log2 (juce::jmax (0.001f, ratio));    // -2 to +2
    return (1.0f - (log + 2.0f) / 4.0f) * (float) getHeight();
}
float PitchSeqEditor::ratioFromY (float y) const
{
    float norm = 1.0f - y / juce::jmax (1.0f, (float) getHeight());
    return std::pow (2.0f, norm * 4.0f - 2.0f);            // 0.25 to 4.0
}
float PitchSeqEditor::snapRatio (float ratio) const
{
    if (! snapEnabled) return ratio;
    float best = kSnapRatios[0];
    float bestDist = std::abs (std::log2 (ratio) - std::log2 (best));
    for (float s : kSnapRatios)
    {
        float d = std::abs (std::log2 (ratio) - std::log2 (s));
        if (d < bestDist) { bestDist = d; best = s; }
    }
    return best;
}

int PitchSeqEditor::nearestPoint (float x, float y) const
{
    int   best = -1;
    float bestDist = 10.0f * 10.0f;  // 10 px radius squared
    for (int i = 0; i < pattern.numPoints; ++i)
    {
        float dx = xFromBeat (pattern.points[i].timeBeat) - x;
        float dy = yFromRatio (pattern.points[i].pitchRatio) - y;
        float d  = dx * dx + dy * dy;
        if (d < bestDist) { bestDist = d; best = i; }
    }
    return best;
}

void PitchSeqEditor::notifyChange()  { if (onPatternChanged) onPatternChanged (pattern); }
void PitchSeqEditor::setPattern      (const SeqPattern& p) { pattern = p; repaint(); }
void PitchSeqEditor::setPlayheadBeat (float beat)          { if (playheadBeat != beat) { playheadBeat = beat; repaint(); } }

void PitchSeqEditor::paint (juce::Graphics& g)
{
    const float w = (float) getWidth();
    const float h = (float) getHeight();

    g.fillAll (juce::Colour (0xff1a1a1a));

    // Horizontal reference lines at each snap pitch
    for (int i = 0; i < 5; ++i)
    {
        float ry = yFromRatio (kSnapRatios[i]);
        bool isUnison = (i == 2);
        g.setColour (isUnison ? juce::Colour (0xff2a2a2a) : juce::Colour (0xff222222));
        g.drawHorizontalLine ((int) ry, 0.0f, w);

        // Labels
        g.setColour (juce::Colour (0xff444444));
        g.setFont (juce::FontOptions (9.0f));
        juce::String lbl = (kSnapLabels[i] >= 0 ? "+" : "") + juce::String ((int) kSnapLabels[i]);
        g.drawText (lbl, 3, (int) ry - 9, 20, 10, juce::Justification::left, false);
    }

    // Vertical beat grid
    g.setColour (juce::Colour (0xff242424));
    for (float b = 1.0f; b < pattern.patternBeats; b += 1.0f)
        g.drawVerticalLine ((int) xFromBeat (b), 0.0f, h);

    // Pattern-end marker
    g.setColour (juce::Colour (0xff333333));
    g.drawVerticalLine ((int) w - 1, 0.0f, h);

    // Polyline between breakpoints
    if (pattern.numPoints >= 2)
    {
        // Main curve (sorted points)
        juce::Path curve;
        curve.startNewSubPath (xFromBeat (pattern.points[0].timeBeat),
                               yFromRatio (pattern.points[0].pitchRatio));
        for (int i = 1; i < pattern.numPoints; ++i)
            curve.lineTo (xFromBeat (pattern.points[i].timeBeat),
                          yFromRatio (pattern.points[i].pitchRatio));

        g.setColour (juce::Colour (0xff00e676));
        g.strokePath (curve, juce::PathStrokeType (1.5f));

        // Wrap segment (last point → right edge, dashed; left edge → first point, dashed)
        {
            const auto& last  = pattern.points[pattern.numPoints - 1];
            const auto& first = pattern.points[0];
            float x0 = xFromBeat (last.timeBeat),  y0 = yFromRatio (last.pitchRatio);
            float x1 = w,                           y1 = yFromRatio (first.pitchRatio);
            float x2 = 0.0f,                        y2 = yFromRatio (first.pitchRatio);
            float x3 = xFromBeat (first.timeBeat),  y3 = yFromRatio (first.pitchRatio);

            juce::Path wrap;
            wrap.startNewSubPath (x0, y0);  wrap.lineTo (x1, y1);
            wrap.startNewSubPath (x2, y2);  wrap.lineTo (x3, y3);

            juce::PathStrokeType dashed (1.0f);
            float dashLengths[] = { 4.0f, 4.0f };
            g.setColour (juce::Colour (0xff00e676).withAlpha (0.3f));
            g.strokePath (wrap, dashed);
        }
    }
    else if (pattern.numPoints == 1)
    {
        // Single point: just draw a horizontal line at that pitch
        float y0 = yFromRatio (pattern.points[0].pitchRatio);
        g.setColour (juce::Colour (0xff00e676).withAlpha (0.4f));
        g.drawHorizontalLine ((int) y0, 0.0f, w);
    }

    // Control point circles
    for (int i = 0; i < pattern.numPoints; ++i)
    {
        float cx = xFromBeat (pattern.points[i].timeBeat);
        float cy = yFromRatio (pattern.points[i].pitchRatio);
        bool isDrag = (i == dragPoint);
        g.setColour (isDrag ? juce::Colours::white : juce::Colour (0xff00e676));
        g.fillEllipse (cx - 5.0f, cy - 5.0f, 10.0f, 10.0f);
        g.setColour (juce::Colour (0xff1a1a1a));
        g.fillEllipse (cx - 3.0f, cy - 3.0f, 6.0f, 6.0f);
    }

    // Playhead
    {
        float phx = xFromBeat (playheadBeat);
        g.setColour (juce::Colour (0xff00e676).withAlpha (0.5f));
        g.drawVerticalLine ((int) phx, 0.0f, h);
    }

    g.setColour (juce::Colour (0xff333333));
    g.drawRect (getLocalBounds(), 1);
}

void PitchSeqEditor::mouseDown (const juce::MouseEvent& e)
{
    const float x = (float) e.x;
    const float y = (float) e.y;

    if (e.mods.isRightButtonDown())
    {
        int hit = nearestPoint (x, y);
        if (hit >= 0 && pattern.numPoints > 1)
        {
            for (int i = hit; i < pattern.numPoints - 1; ++i)
                pattern.points[i] = pattern.points[i + 1];
            --pattern.numPoints;
            notifyChange();
            repaint();
        }
        return;
    }

    int hit = nearestPoint (x, y);
    if (hit >= 0)
    {
        dragPoint = hit;
    }
    else if (pattern.numPoints < MAX_SEQ_STEPS)
    {
        // Add new point at cursor position
        float beat  = juce::jlimit (0.0f, pattern.patternBeats - 0.0001f, beatFromX (x));
        float ratio = snapRatio (juce::jlimit (0.25f, 4.0f, ratioFromY (y)));
        pattern.points[pattern.numPoints++] = { beat, ratio };
        pattern.sortPoints();
        // Find the new point after sort
        dragPoint = -1;
        for (int i = 0; i < pattern.numPoints; ++i)
            if (std::abs (pattern.points[i].timeBeat - beat) < 0.0001f)
                { dragPoint = i; break; }
        notifyChange();
        repaint();
    }
}

void PitchSeqEditor::mouseDrag (const juce::MouseEvent& e)
{
    if (dragPoint < 0) return;

    float beat  = juce::jlimit (0.0f, pattern.patternBeats - 0.0001f, beatFromX ((float) e.x));
    float ratio = snapRatio (juce::jlimit (0.25f, 4.0f, ratioFromY ((float) e.y)));

    pattern.points[dragPoint].timeBeat   = beat;
    pattern.points[dragPoint].pitchRatio = ratio;
    pattern.sortPoints();

    // Re-find drag point after sort (it may have moved index)
    for (int i = 0; i < pattern.numPoints; ++i)
        if (std::abs (pattern.points[i].timeBeat - beat) < 0.0001f &&
            std::abs (pattern.points[i].pitchRatio - ratio) < 0.00001f)
            { dragPoint = i; break; }

    notifyChange();
    repaint();
}

void PitchSeqEditor::mouseUp (const juce::MouseEvent&)
{
    dragPoint = -1;
}


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
      // seqLengthAttachment  (p.apvts, "seq_length", seqLengthSlider),  // old APVTS attachment
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
    dryWetSlider.setSliderStyle (juce::Slider::LinearVertical);
    dryWetSlider.setTextBoxStyle (juce::Slider::NoTextBox, false, 0, 0);
    addAndMakeVisible (dryWetSlider);
    dryWetLabel.setText ("DRY/WET", juce::dontSendNotification);
    dryWetLabel.setJustificationType (juce::Justification::centred);
    dryWetLabel.setFont (juce::FontOptions (9.0f));
    addAndMakeVisible (dryWetLabel);

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

    // ---- Old slider-based sequencer setup (replaced by PitchSeqEditor) ----
    // seqHeaderLabel / seqLengthSlider / seqStepSliders / seqStepAttachments setup removed.
    // Code preserved in PluginEditor.h and the old GranularProcessor APVTS paths above.

    // Graphical pitch envelope
    seqHeaderLabel.setText ("PITCH ENV", juce::dontSendNotification);
    seqHeaderLabel.setJustificationType (juce::Justification::left);
    seqHeaderLabel.setFont (juce::FontOptions (10.0f));
    seqHeaderLabel.setColour (juce::Label::textColourId, juce::Colour (0xff888888));
    addAndMakeVisible (seqHeaderLabel);

    seqEditor.setPattern (p.getPattern());
    seqEditor.onPatternChanged = [&p] (const SeqPattern& pat) { p.setPattern (pat); };
    addAndMakeVisible (seqEditor);

    snapButton.setClickingTogglesState (true);
    snapButton.onClick = [this] { seqEditor.snapEnabled = snapButton.getToggleState(); };
    addAndMakeVisible (snapButton);

    {
        const float pb = p.getPattern().patternBeats;
        patternLenButton.setButtonText (juce::String ((int) pb) + " BEATS");
    }
    patternLenButton.onClick = [this, &p]
    {
        static const float kLengths[] = { 1.0f, 2.0f, 4.0f, 8.0f };
        SeqPattern pat = p.getPattern();
        int idx = 2;
        for (int i = 0; i < 4; ++i)
            if (std::abs (pat.patternBeats - kLengths[i]) < 0.01f) idx = i;
        idx = (idx + 1) % 4;
        pat.patternBeats = kLengths[idx];
        for (int i = 0; i < pat.numPoints; ++i)
            pat.points[i].timeBeat = juce::jlimit (0.0f, pat.patternBeats - 0.0001f, pat.points[i].timeBeat);
        pat.sortPoints();
        p.setPattern (pat);
        seqEditor.setPattern (pat);
        patternLenButton.setButtonText (juce::String ((int) kLengths[idx]) + " BEATS");
    };
    addAndMakeVisible (patternLenButton);

    // Cache sync param pointers so timerCallback can read them without a hash lookup.
    densitySyncParam = p.apvts.getRawParameterValue ("density_sync");
    sizeSyncParam    = p.apvts.getRawParameterValue ("size_sync");

    // Poll every ~100 ms to grey out / restore dials as sync toggles change.
    startTimerHz (10);

    setSize (DW_W + PADDING + (NUM_CONTROLS + 1) * (DIAL_SIZE + PADDING),
             PADDING + LABEL_HEIGHT + DIAL_SIZE + 16   // dial row
             + PADDING + 20 + 4 + 20                  // sync row (toggle + gap + combo)
             + PADDING + LABEL_HEIGHT + 120 + PADDING); // env header + editor + bottom
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

    seqEditor.setPlayheadBeat (audioProcessor.getPlayheadBeat());
}

//==============================================================================
void Granular_fx_testAudioProcessorEditor::paint (juce::Graphics& g)
{
    g.fillAll (getLookAndFeel().findColour (juce::ResizableWindow::backgroundColourId));
}

void Granular_fx_testAudioProcessorEditor::resized()
{
    // Dry/wet slider — full height on the far left.
    dryWetLabel.setBounds  (0, PADDING, DW_W, LABEL_HEIGHT);
    dryWetSlider.setBounds (0, PADDING + LABEL_HEIGHT, DW_W,
                            getHeight() - PADDING - LABEL_HEIGHT - PADDING);

    int x = DW_W + PADDING;
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

    const int sizeX = x;
    placeControl (grainSizeSlider, grainSizeLabel);

    const int densityX = x;
    placeControl (grainDensitySlider, grainDensityLabel);

    placeControl (positionScatterSlider, positionScatterLabel);
    placeControl (sizeScatterSlider,     sizeScatterLabel);
    placeControl (panScatterSlider,      panScatterLabel);

    // Reverse toggle — label above, button centred in the slot below.
    reverseLabel.setBounds  (x, y, DIAL_SIZE, LABEL_HEIGHT);
    reverseButton.setBounds (x + DIAL_SIZE / 4, y + LABEL_HEIGHT + (DIAL_SIZE / 2) - 10,
                             DIAL_SIZE / 2, 20);

    // Sync controls: toggle and combo each span the full dial-slot width.
    sizeSyncButton.setBounds    (sizeX,    syncRowY,                      DIAL_SIZE, toggleH);
    sizeDivisionBox.setBounds   (sizeX,    syncRowY + toggleH + comboGap,  DIAL_SIZE, comboH);

    densitySyncButton.setBounds (densityX, syncRowY,                      DIAL_SIZE, toggleH);
    densityDivisionBox.setBounds(densityX, syncRowY + toggleH + comboGap,  DIAL_SIZE, comboH);

    // Graphical pitch envelope — below the sync controls.
    const int seqRowY     = syncRowY + toggleH + comboGap + comboH + PADDING;
    const int seqEditorH  = 120;
    const int dialAreaL   = DW_W + PADDING;
    const int editorWidth = getWidth() - dialAreaL - PADDING;

    // Header row: label on left, length button and snap button on right
    seqHeaderLabel.setBounds   (dialAreaL,                             seqRowY, editorWidth - 160, LABEL_HEIGHT);
    patternLenButton.setBounds (getWidth() - PADDING - 100 - 4 - 56,  seqRowY, 100, LABEL_HEIGHT);
    snapButton.setBounds       (getWidth() - PADDING - 56,             seqRowY,  56, LABEL_HEIGHT);

    seqEditor.setBounds (dialAreaL, seqRowY + LABEL_HEIGHT, editorWidth, seqEditorH);
}
