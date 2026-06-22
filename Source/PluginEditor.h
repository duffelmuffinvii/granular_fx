/*
  ==============================================================================

    This file contains the basic framework code for a JUCE plugin editor.

  ==============================================================================
*/

#pragma once

#include <JuceHeader.h>
#include "PluginProcessor.h"

// ============================================================================
// GranularLookAndFeel
//
// A custom LookAndFeel that overrides the colours JUCE uses when drawing
// sliders, rotary knobs, text boxes, and labels. Inheriting from
// LookAndFeel_V4 means everything not overridden here falls back to the
// standard JUCE appearance.
//
// To change the colour scheme, edit the Colour values in the constructor.
// Colours are specified as 0xAARRGGBB hex values (AA = alpha/opacity,
// RR/GG/BB = red/green/blue). 0xff = fully opaque.
// ============================================================================

class GranularLookAndFeel : public juce::LookAndFeel_V4
{
public:
    GranularLookAndFeel()
    {
        // ----------------------------------------------------------------
        // COLOUR PALETTE — edit these to restyle the whole plugin.
        // ----------------------------------------------------------------
        const juce::Colour background  (0xff1c1c1c);  // very dark grey
        const juce::Colour trackBg     (0xff2e2e2e);  // slightly lighter for grooves
        const juce::Colour thumb       (0xff888888);  // mid grey for knobs/thumb
        const juce::Colour outline     (0xff444444);  // subtle grey border
        const juce::Colour textColour  (0xffdddddd);  // light grey for labels and values
        const juce::Colour highlight   (0xff00e676);  // green accent for indicator lines
        // ----------------------------------------------------------------

        // Window / component background
        setColour (juce::ResizableWindow::backgroundColourId, background);

        // Rotary knob fill, outline, and pointer line
        setColour (juce::Slider::rotarySliderFillColourId,    thumb);
        setColour (juce::Slider::rotarySliderOutlineColourId, trackBg);
        setColour (juce::Slider::thumbColourId,               thumb);

        // Linear slider track (background groove) and filled portion
        setColour (juce::Slider::trackColourId,               trackBg);
        setColour (juce::Slider::backgroundColourId,          background);

        // Labels
        setColour (juce::Label::textColourId,                 textColour);
        setColour (juce::Label::backgroundColourId,           juce::Colours::transparentBlack);
    }

    // ----------------------------------------------------------------
    // drawRotarySlider
    // Called by JUCE every time a rotary knob needs to be repainted.
    // This draws a clean flat arc style: a grey track ring with a
    // filled arc showing the current value, and a dot for the pointer.
    // ----------------------------------------------------------------
    void drawLinearSlider (juce::Graphics& g,
                           int x, int y, int width, int height,
                           float sliderPos, float /*minPos*/, float /*maxPos*/,
                           juce::Slider::SliderStyle style, juce::Slider& slider) override
    {
        if (style != juce::Slider::LinearVertical)
        {
            LookAndFeel_V4::drawLinearSlider (g, x, y, width, height,
                                              sliderPos, 0, 0, style, slider);
            return;
        }

        const float centreX   = x + width * 0.5f;
        const float trackW    = 4.0f;   // width of the groove
        const float thumbW    = 22.0f;  // width of the fader cap
        const float thumbH    = 14.0f;  // height of the fader cap

        // --- Groove: inset dark track running the full height ---
        const float trackX = centreX - trackW * 0.5f;
        g.setColour (juce::Colour (0xff111111));
        g.fillRoundedRectangle (trackX, (float) y, trackW, (float) height, trackW * 0.5f);

        // Groove highlight on left edge to simulate inset depth
        g.setColour (juce::Colours::white.withAlpha (0.06f));
        g.fillRoundedRectangle (trackX, (float) y, 1.0f, (float) height, 0.5f);

        // Groove shadow on right edge
        g.setColour (juce::Colours::black.withAlpha (0.3f));
        g.fillRoundedRectangle (trackX + trackW - 1.0f, (float) y, 1.0f, (float) height, 0.5f);

        // --- Thumb cap ---
        const float thumbX = centreX - thumbW * 0.5f;
        const float thumbY = sliderPos - thumbH * 0.5f;

        // Drop shadow
        g.setColour (juce::Colours::black.withAlpha (0.45f));
        g.fillRoundedRectangle (thumbX + 1.5f, thumbY + 2.5f, thumbW, thumbH, 3.0f);

        // Body
        g.setColour (juce::Colour (0xff2e2e2e));
        g.fillRoundedRectangle (thumbX, thumbY, thumbW, thumbH, 3.0f);

        // Highlight on top edge
        g.setColour (juce::Colours::white.withAlpha (0.18f));
        g.fillRoundedRectangle (thumbX + 1.0f, thumbY + 1.0f, thumbW - 2.0f, 2.0f, 1.0f);

        // Shadow on bottom edge
        g.setColour (juce::Colours::black.withAlpha (0.4f));
        g.fillRoundedRectangle (thumbX + 1.0f, thumbY + thumbH - 3.0f, thumbW - 2.0f, 2.0f, 1.0f);

        // Center indicator line
        const float midY = thumbY + thumbH * 0.5f;
        g.setColour (juce::Colour (0xff00e676));
        g.drawLine (thumbX + 4.0f, midY, thumbX + thumbW - 4.0f, midY, 1.0f);
    }

    void drawRotarySlider (juce::Graphics& g,
                           int x, int y, int width, int height,
                           float sliderPos,
                           float rotaryStartAngle,
                           float rotaryEndAngle,
                           juce::Slider&) override
    {
        const float radius  = juce::jmin (width, height) * 0.5f - 6.0f;
        const float centreX = x + width  * 0.5f;
        const float centreY = y + height * 0.5f;
        const float angle   = rotaryStartAngle + sliderPos * (rotaryEndAngle - rotaryStartAngle);

        // --- Track ring showing full parameter range ---
        //{
        //    juce::Path track;
        //    track.addCentredArc (centreX, centreY, radius + 4.0f, radius + 4.0f,
        //                         0.0f, rotaryStartAngle, rotaryEndAngle, true);
        //    g.setColour (juce::Colour (0xff3a3a3a));
        //    g.strokePath (track, juce::PathStrokeType (2.5f, juce::PathStrokeType::curved,
        //                                               juce::PathStrokeType::rounded));
        //}

        // --- Value arc showing current position ---
        //{
        //    juce::Path value;
        //    value.addCentredArc (centreX, centreY, radius + 4.0f, radius + 4.0f,
        //                         0.0f, rotaryStartAngle, angle, true);
        //    g.setColour (juce::Colour (0xff888888));
        //    g.strokePath (value, juce::PathStrokeType (2.5f, juce::PathStrokeType::curved,
        //                                               juce::PathStrokeType::rounded));
        //}

        // --- Drop shadow (offset ellipse below the knob) ---
        g.setColour (juce::Colours::black.withAlpha (0.5f));
        g.fillEllipse (centreX - radius + 2.0f, centreY - radius + 3.0f,
                       radius * 2.0f, radius * 2.0f);

        // --- Main knob body ---
        g.setColour (juce::Colour (0xff2a2a2a));
        g.fillEllipse (centreX - radius, centreY - radius, radius * 2.0f, radius * 2.0f);

        // --- Bevel highlight: lighter arc on top-left (light source top-left) ---
        {
            juce::Path highlight;
            highlight.addCentredArc (centreX, centreY, radius - 1.0f, radius - 1.0f,
                                     0.0f,
                                     -juce::MathConstants<float>::pi * 0.85f,
                                      juce::MathConstants<float>::pi * 0.15f,
                                     true);
            g.setColour (juce::Colours::white.withAlpha (0.18f));
            g.strokePath (highlight, juce::PathStrokeType (1.5f));
        }

        // --- Bevel shadow: darker arc on bottom-right ---
        {
            juce::Path shadow;
            shadow.addCentredArc (centreX, centreY, radius - 1.0f, radius - 1.0f,
                                  0.0f,
                                   juce::MathConstants<float>::pi * 0.15f,
                                   juce::MathConstants<float>::pi * 1.15f,
                                  true);
            g.setColour (juce::Colours::black.withAlpha (0.4f));
            g.strokePath (shadow, juce::PathStrokeType (1.5f));
        }

        // --- Indicator line ---
        const float sinA      = std::sin (angle);
        const float cosA      = std::cos (angle);
        const float lineInner = radius * 0.25f;
        const float lineOuter = radius * 0.82f;

        g.setColour (juce::Colour (0xff00e676));
        g.drawLine (centreX + sinA * lineInner, centreY - cosA * lineInner,
                    centreX + sinA * lineOuter, centreY - cosA * lineOuter,
                    1.5f);
    }


};


//==============================================================================
class Granular_fx_testAudioProcessorEditor  : public juce::AudioProcessorEditor,
                                               private juce::Timer
{
public:
    Granular_fx_testAudioProcessorEditor (Granular_fx_testAudioProcessor&);
    ~Granular_fx_testAudioProcessorEditor() override;

    //==============================================================================
    void paint (juce::Graphics&) override;
    void resized() override;

private:
    void timerCallback() override;

    Granular_fx_testAudioProcessor& audioProcessor;

    // The custom look and feel. Declared first so it exists before any
    // component tries to use it during construction.
    GranularLookAndFeel lookAndFeel;

    juce::Slider grainSizeSlider;
    juce::Slider grainDensitySlider;
    juce::Slider pitchRatioSlider;
    juce::Slider positionScatterSlider;
    juce::Slider sizeScatterSlider;
    juce::Slider panScatterSlider;
    juce::Slider dryWetSlider;

    juce::Label grainSizeLabel;
    juce::Label grainDensityLabel;
    juce::Label pitchRatioLabel;
    juce::Label positionScatterLabel;
    juce::Label sizeScatterLabel;
    juce::Label panScatterLabel;
    juce::Label dryWetLabel;

    juce::ToggleButton reverseButton;
    juce::Label        reverseLabel;

    // Pitch probability weights — one dial per octave option
    juce::Label  pitchWeightHeaderLabel;
    juce::Slider pitchWeightSliders[5];
    juce::Label  pitchWeightLabels[5];

    // Tempo sync controls — one toggle + one division combo per synced parameter.
    juce::ToggleButton densitySyncButton;
    juce::ToggleButton sizeSyncButton;
    juce::ComboBox     densityDivisionBox;
    juce::ComboBox     sizeDivisionBox;

    // Cached pointers for the sync bool params (read in timerCallback).
    std::atomic<float>* densitySyncParam = nullptr;
    std::atomic<float>* sizeSyncParam    = nullptr;

    using SliderAttachment   = juce::AudioProcessorValueTreeState::SliderAttachment;
    using ButtonAttachment   = juce::AudioProcessorValueTreeState::ButtonAttachment;
    using ComboBoxAttachment = juce::AudioProcessorValueTreeState::ComboBoxAttachment;
    SliderAttachment   grainSizeAttachment;
    SliderAttachment   grainDensityAttachment;
    SliderAttachment   pitchRatioAttachment;
    SliderAttachment   positionScatterAttachment;
    SliderAttachment   sizeScatterAttachment;
    SliderAttachment   panScatterAttachment;
    SliderAttachment   dryWetAttachment;
    ButtonAttachment   reverseAttachment;
    std::unique_ptr<SliderAttachment> pitchWeightAttachments[5];
    ButtonAttachment   densitySyncAttachment;
    ButtonAttachment   sizeSyncAttachment;
    ComboBoxAttachment densityDivisionAttachment;
    ComboBoxAttachment sizeDivisionAttachment;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (Granular_fx_testAudioProcessorEditor)
};
