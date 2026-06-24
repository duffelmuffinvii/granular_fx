/*
  ==============================================================================
  GranularProcessor.h
  ==============================================================================

  This file implements a granular audio effect.

  HOW GRANULAR SYNTHESIS WORKS (brief overview):
  -----------------------------------------------
  Instead of playing audio straight through, granular processing chops the
  incoming audio into tiny fragments called "grains" (typically 20-200ms).
  Each grain is faded in and out with a smooth window to avoid clicks, then
  many grains are layered together and played back simultaneously — often
  with slight random variations in timing, position, pitch, and panning.

  HOW PARAMETERS WORK IN THIS VERSION:
  -----------------------------------------------
  Parameters are now managed by an AudioProcessorValueTreeState (APVTS),
  which lives in PluginProcessor. This means:

    - The DAW can automate every parameter.
    - Parameter values are saved and restored with the project.
    - Values can be changed in real time without recompiling.

  To change default values or ranges, find createParameterLayout() in
  PluginProcessor.cpp — that's where each parameter's min, max, and
  default are defined.

  PARAMETER IDs (use these strings to identify each parameter):
  -----------------------------------------------
    "grain_size"        — grain duration in milliseconds      (10–500 ms)
    "grain_density"     — grains started per second           (1–100)
"position_scatter"  — random variation in start position  (0–1)
    "size_scatter"      — random variation in grain duration  (0–1)
    "pan_scatter"       — random variation in stereo pan      (0–1)
    "dry_wet"           — dry/wet mix                         (0–1)

  FIXED CONSTANTS (require recompile to change):
  -----------------------------------------------
  These affect memory allocation so they can't be runtime parameters.

  ==============================================================================
*/

#pragma once

#include <JuceHeader.h>
#include <array>
#include <random>

// How many seconds of incoming audio to store in the recording buffer.
// Grains read their audio from this buffer. More = richer temporal scatter,
// but uses more memory (~2 * sampleRate * BUFFER_DURATION_SECONDS * 4 bytes).
static constexpr float BUFFER_DURATION_SECONDS = 2.0f;

// Maximum number of grains playing simultaneously.
// If all slots are occupied, new grains are skipped until one finishes.
static constexpr int MAX_GRAINS = 64;

// Resolution of the pre-computed Hann window lookup table.
// 1024 points gives sub-millisecond accuracy — more than sufficient for envelope shaping.
static constexpr int HANN_TABLE_SIZE = 1024;


// ============================================================
//  INTERNAL IMPLEMENTATION
// ============================================================

// ------------------------------------------------------------
// Grain
// Represents one short fragment of audio currently being played.
// GranularProcessor maintains a fixed pool of MAX_GRAINS of these.
// ------------------------------------------------------------
struct Grain
{
    // Where we are currently reading in the circular buffer.
    // This is a float so we can advance by fractional amounts (for pitch shifting).
    float readPos         = 0.0f;

    // How fast to advance readPos each sample.
    //   1.0 = normal speed/pitch, 2.0 = double speed (up an octave), etc.
    float speed           = 1.0f;

    // Total length of this grain in samples.
    int   durationSamples = 0;

    // How many samples of this grain have been played so far.
    // When age == durationSamples, the grain is finished.
    int   age             = 0;

    // Stereo pan position: -1.0 = hard left, 0.0 = center, +1.0 = hard right.
    float pan             = 0.0f;

    // Constant-power pan gains, computed once at spawn to avoid per-sample trig.
    float leftGain        = 1.0f;
    float rightGain       = 0.0f;

    // Whether this grain slot is currently playing. False = available for reuse.
    bool  active          = false;
};


// ------------------------------------------------------------
// GranularProcessor
// The main class. Owns a circular recording buffer and a pool
// of grains. Call prepareToPlay() once, then processBlock()
// every audio block.
// ------------------------------------------------------------
class GranularProcessor
{
public:

    GranularProcessor() : rng (std::random_device{}()) {}

    // --------------------------------------------------------
    // prepareToPlay
    // Called by the host before playback starts.
    // Stores the APVTS reference so parameters can be read at runtime,
    // caches the atomic float pointers for lock-free audio-thread access,
    // allocates the circular buffer, and resets all grain state.
    //
    // WHY cache pointers instead of calling getRawParameterValue() each sample?
    // getRawParameterValue() does a string lookup in a hash map — that's fine
    // to call once at setup, but too slow to call thousands of times per second
    // on the audio thread. Caching the pointer means the audio thread just reads
    // a single atomic float, which is extremely fast.
    // --------------------------------------------------------
    void prepareToPlay (double sampleRate, int maxBlockSize,
                        juce::AudioProcessorValueTreeState& apvts)
    {
        currentSampleRate = sampleRate;

        // Cache pointers to each parameter's atomic float.
        // The audio thread reads these via ->load() in processBlock / spawnGrain.
        grainSizeParam       = apvts.getRawParameterValue ("grain_size");
        grainDensityParam    = apvts.getRawParameterValue ("grain_density");
        seqLengthParam = apvts.getRawParameterValue ("seq_length");
        for (int i = 0; i < 8; ++i)
            seqStepParams[i] = apvts.getRawParameterValue ("seq_step_" + juce::String (i));
        positionScatterParam = apvts.getRawParameterValue ("position_scatter");
        sizeScatterParam     = apvts.getRawParameterValue ("size_scatter");
        panScatterParam      = apvts.getRawParameterValue ("pan_scatter");
        dryWetParam          = apvts.getRawParameterValue ("dry_wet");
        reverseParam         = apvts.getRawParameterValue ("reverse");
        densitySyncParam     = apvts.getRawParameterValue ("density_sync");
        sizeSyncParam        = apvts.getRawParameterValue ("size_sync");
        densityDivisionParam = apvts.getRawParameterValue ("density_division");
        sizeDivisionParam    = apvts.getRawParameterValue ("size_division");
        // Allocate a stereo circular buffer long enough to hold BUFFER_DURATION_SECONDS.
        int bufLenSamples = static_cast<int> (sampleRate * BUFFER_DURATION_SECONDS);
        circularBuffer.setSize (2, bufLenSamples);
        circularBuffer.clear();
        writePos = 0;

        // Pre-allocate wet buffer to avoid heap allocation on the audio thread.
        wetBuffer.setSize (2, maxBlockSize);
        wetBuffer.clear();

        // Pre-compute the Hann window lookup table.
        for (int i = 0; i < HANN_TABLE_SIZE; ++i)
            hannTable[i] = 0.5f * (1.0f - std::cos (
                juce::MathConstants<float>::twoPi * i / HANN_TABLE_SIZE));

        // Mark all grain slots as free.
        for (auto& g : grains)
            g.active = false;

        samplesSinceLastGrain = 0.0f;
        currentStep = 0;
    }

    // --------------------------------------------------------
    // processBlock
    // Called every audio block by the host.
    //
    //  buffer      - the audio to process (read input from here,
    //                write processed output back to here)
    //  numChannels - number of active channels (usually 1 or 2)
    //
    // This function:
    //   1. Reads current parameter values from the APVTS atomics.
    //   2. Records input audio into the circular buffer.
    //   3. Spawns new grains on a timed schedule.
    //   4. Advances all active grains and sums their output.
    //   5. Mixes the granular result with the dry signal.
    // --------------------------------------------------------
    void processBlock (juce::AudioBuffer<float>& buffer, int numChannels, double bpm = 120.0)
    {
        // Guard against prepareToPlay not having been called yet.
        if (grainSizeParam == nullptr)
            return;

        const int numSamples = buffer.getNumSamples();
        const int bufLen     = circularBuffer.getNumSamples();

        // Division table: each entry is a fraction of a quarter note (beat).
        // duration_ms  = fraction * 60000 / bpm
        // density_grains_per_sec = bpm / (fraction * 60)
        static constexpr float divFraction[] = { 0.125f, 0.25f, 0.5f, 1.0f, 2.0f, 4.0f };

        // Read all parameter values once per block.
        // .load() atomically reads the float — safe to call from the audio thread.
        const bool densitySync = densitySyncParam->load() >= 0.5f;
        const bool sizeSync    = sizeSyncParam->load()    >= 0.5f;

        float grainSizeMs;
        if (sizeSync)
        {
            int idx      = juce::jlimit (0, 5, static_cast<int> (sizeDivisionParam->load()));
            grainSizeMs  = divFraction[idx] * 60000.0f / static_cast<float> (bpm);
            grainSizeMs  = juce::jlimit (10.0f, 500.0f, grainSizeMs);
        }
        else
        {
            grainSizeMs = grainSizeParam->load();
        }

        float grainDensity;
        if (densitySync)
        {
            int idx      = juce::jlimit (0, 5, static_cast<int> (densityDivisionParam->load()));
            grainDensity = static_cast<float> (bpm) / (divFraction[idx] * 60.0f);
            grainDensity = juce::jlimit (1.0f, 100.0f, grainDensity);
        }
        else
        {
            grainDensity = grainDensityParam->load();
        }
        const bool  reverse          = reverseParam->load() >= 0.5f;

        // Pitch table shared by the weighted random selection below.
        static constexpr float pitchTable[] = { 0.25f, 0.5f, 1.0f, 2.0f, 4.0f };

        const float positionScatter  = positionScatterParam->load();
        const float sizeScatter      = sizeScatterParam->load();
        const float panScatter       = panScatterParam->load();
        const float dryWet           = dryWetParam->load();

        // How many samples to wait between spawning grains.
        // e.g. at 44100 Hz and density=15, this is 44100/15 = 2940 samples.
        const float samplesPerGrain = static_cast<float> (currentSampleRate) / grainDensity;

        // Normalise the accumulated wet signal so that stacking many overlapping grains
        // doesn't multiply the output level. size_scatter can inflate actual grain
        // duration up to (1 + sizeScatter)x the base, so factor that in too.
        const float expectedActive = grainDensity * (grainSizeMs / 1000.0f) * (1.0f + sizeScatter);
        const float wetGain = 1.0f / juce::jmax (1.0f, expectedActive);

        // Clear the pre-allocated wet buffer for this block's accumulation.
        wetBuffer.clear();

        for (int s = 0; s < numSamples; ++s)
        {
            // Step 1: Record the current input sample into the circular buffer.
            for (int ch = 0; ch < juce::jmin (numChannels, 2); ++ch)
                circularBuffer.setSample (ch, writePos, buffer.getSample (ch, s));

            writePos = (writePos + 1) % bufLen;

            // Step 2: Check if it's time to spawn a new grain.
            samplesSinceLastGrain += 1.0f;
            if (samplesSinceLastGrain >= samplesPerGrain)
            {
                samplesSinceLastGrain -= samplesPerGrain;

                // Stepped sequencer pitch selection.
                const int seqLen = juce::jlimit (1, 8, static_cast<int> (seqLengthParam->load()));
                const int stepIdx = juce::jlimit (0, 4, static_cast<int> (seqStepParams[currentStep]->load()));
                const float selectedPitch = pitchTable[stepIdx];
                currentStep = (currentStep + 1) % seqLen;

                spawnGrain (bufLen, grainSizeMs, selectedPitch, positionScatter, sizeScatter, panScatter, reverse);
            }

            // Step 3: Tick every active grain and add its output to wetBuffer.
            for (auto& grain : grains)
            {
                if (!grain.active)
                    continue;

                // ---- Hann window (table lookup) ----
                // Maps grain progress [0, 1) to a pre-computed Hann value.
                // Avoids calling std::cos on the audio thread.
                float phase    = static_cast<float> (grain.age) / static_cast<float> (grain.durationSamples);
                int   tableIdx = static_cast<int> (phase * (HANN_TABLE_SIZE - 1));
                float window   = hannTable[tableIdx];

                // ---- Linear interpolation ----
                // readPos is a float (e.g. 1234.7). Read the two surrounding
                // integer samples and blend between them to avoid aliasing.
                int   idx0 = static_cast<int> (grain.readPos) % bufLen;
                int   idx1 = (idx0 + 1) % bufLen;
                float frac = grain.readPos - std::floor (grain.readPos);

                float rawL = circularBuffer.getSample (0, idx0)
                           + frac * (circularBuffer.getSample (0, idx1)
                                   - circularBuffer.getSample (0, idx0));

                float rawR = (numChannels > 1)
                    ? circularBuffer.getSample (1, idx0)
                        + frac * (circularBuffer.getSample (1, idx1)
                                - circularBuffer.getSample (1, idx0))
                    : rawL;

                float outL = window * rawL;
                float outR = window * rawR;

                if (numChannels >= 2)
                {
                    wetBuffer.addSample (0, s, outL * grain.leftGain);
                    wetBuffer.addSample (1, s, outR * grain.rightGain);
                }
                else
                {
                    wetBuffer.addSample (0, s, outL);
                }

                // Advance the read head (negative speed = reversed grain).
                grain.readPos += grain.speed;
                if (grain.readPos >= static_cast<float> (bufLen))
                    grain.readPos -= static_cast<float> (bufLen);
                if (grain.readPos < 0.0f)
                    grain.readPos += static_cast<float> (bufLen);

                grain.age++;
                if (grain.age >= grain.durationSamples)
                    grain.active = false;
            }
        }

        // Step 4: Blend wet signal with dry.
        for (int s = 0; s < numSamples; ++s)
        {
            for (int ch = 0; ch < numChannels; ++ch)
            {
                float dry = buffer.getSample (ch, s);
                float wet = wetBuffer.getSample (ch, s);
                buffer.setSample (ch, s, dry * (1.0f - dryWet) + wet * wetGain * dryWet);
            }
        }
    }

private:

    // --------------------------------------------------------
    // spawnGrain
    // Finds an unused grain slot and fills it with new parameters.
    // All the per-grain randomisation happens here.
    // --------------------------------------------------------
    void spawnGrain (int bufLen, float grainSizeMs, float pitchRatio,
                     float positionScatter, float sizeScatter, float panScatter, bool reverse)
    {
        Grain* slot = nullptr;
        for (auto& g : grains)
        {
            if (!g.active) { slot = &g; break; }
        }
        if (slot == nullptr)
            return;

        // Base grain duration in samples.
        float baseDuration = (grainSizeMs / 1000.0f) * static_cast<float> (currentSampleRate);

        // Randomise duration by ±sizeScatter.
        float r0 = static_cast<float> (rng()) / static_cast<float> (0xFFFFFFFFu);
        slot->durationSamples = juce::jmax (1, static_cast<int> (
            baseDuration * (1.0f - sizeScatter + 2.0f * sizeScatter * r0)));

        // Look back from the write head; scatter randomises how far back.
        float r1 = static_cast<float> (rng()) / static_cast<float> (0xFFFFFFFFu);
        int lookback = static_cast<int> (baseDuration + r1 * positionScatter * bufLen);
        lookback = juce::jmin (lookback, bufLen - 1);
        slot->readPos = static_cast<float> ((writePos - lookback + bufLen) % bufLen);

        // Negative speed plays the grain backwards through the buffer.
        slot->speed = reverse ? -pitchRatio : pitchRatio;

        float r2 = static_cast<float> (rng()) / static_cast<float> (0xFFFFFFFFu);
        slot->pan = -panScatter + 2.0f * panScatter * r2;

        float panAngle   = (slot->pan + 1.0f) * 0.25f * juce::MathConstants<float>::pi;
        slot->leftGain   = std::cos (panAngle);
        slot->rightGain  = std::sin (panAngle);

        slot->age    = 0;
        slot->active = true;
    }

    // ---- Cached parameter pointers ----
    // These are set in prepareToPlay and read (via ->load()) in the audio thread.
    // Using raw pointers here is intentional and safe — the APVTS owns the atomics
    // and outlives this object.
    std::atomic<float>* grainSizeParam       = nullptr;
    std::atomic<float>* grainDensityParam    = nullptr;
    std::atomic<float>* positionScatterParam = nullptr;
    std::atomic<float>* sizeScatterParam     = nullptr;
    std::atomic<float>* panScatterParam      = nullptr;
    std::atomic<float>* dryWetParam          = nullptr;
    std::atomic<float>* reverseParam         = nullptr;
    std::atomic<float>* densitySyncParam     = nullptr;
    std::atomic<float>* sizeSyncParam        = nullptr;
    std::atomic<float>* densityDivisionParam = nullptr;
    std::atomic<float>* sizeDivisionParam    = nullptr;
    std::atomic<float>* seqLengthParam        = nullptr;
    std::atomic<float>* seqStepParams[8]      = {};

    // ---- Audio state ----
    int currentStep = 0;

    juce::AudioBuffer<float> circularBuffer;
    int writePos = 0;
    juce::AudioBuffer<float> wetBuffer;
    std::array<float, HANN_TABLE_SIZE> hannTable;
    std::array<Grain, MAX_GRAINS> grains;
    float samplesSinceLastGrain = 0.0f;
    double currentSampleRate = 44100.0;
    std::mt19937 rng;
};
