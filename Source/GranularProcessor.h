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
#include <algorithm>
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

// Maximum steps in the pattern-based sequencer.
static constexpr int MAX_SEQ_STEPS = 32;

// ============================================================
//  PITCH ENVELOPE DATA
// ============================================================

/* ---- Old step-based structs (replaced by breakpoint envelope) ----
struct SeqStep
{
    int   pitchIndex    = 2;
    float durationBeats = 1.0f;
    bool  slide         = false;
};
struct SeqPattern (old)
{
    SeqStep steps[MAX_SEQ_STEPS];
    int     numSteps = 4;
    SeqPattern() { for (auto& s : steps) s = SeqStep{}; numSteps = 4; }
};
---- END OLD ---- */

struct BreakPoint
{
    float timeBeat   = 0.0f;    // position within pattern in beats [0, patternBeats)
    float pitchRatio = 1.0f;    // continuous pitch ratio: 0.25 (-2oct) to 4.0 (+2oct)
};

struct SeqPattern
{
    BreakPoint points[MAX_SEQ_STEPS];
    int   numPoints    = 2;
    float patternBeats = 4.0f;

    SeqPattern()
    {
        numPoints    = 2;
        patternBeats = 4.0f;
        points[0] = { 0.0f, 1.0f };
        points[1] = { 2.0f, 1.0f };
        for (int i = 2; i < MAX_SEQ_STEPS; ++i)
            points[i] = BreakPoint{};
    }

    void sortPoints()
    {
        std::sort (points, points + numPoints,
            [] (const BreakPoint& a, const BreakPoint& b) { return a.timeBeat < b.timeBeat; });
    }
};


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

    void setPattern (const SeqPattern& p)
    {
        juce::ScopedLock sl (patternLock);
        pendingPattern = p;
        patternDirty   = true;
    }

    SeqPattern getPattern() const
    {
        juce::ScopedLock sl (patternLock);
        return pendingPattern;
    }

    float getPlayheadBeat() const { return playheadBeat.load(); }

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
        // (old stepped-sequencer APVTS params removed — pattern now lives outside APVTS)
        // seqLengthParam = apvts.getRawParameterValue ("seq_length");
        // for (int i = 0; i < 8; ++i)
        //     seqStepParams[i] = apvts.getRawParameterValue ("seq_step_" + juce::String (i));
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
        // currentStep = 0;  // old stepped-sequencer counter — replaced by patternBeatPos
        patternBeatPos = 0.0f;
        playheadBeat.store (0.0f);

        // Push default pattern to audio thread immediately.
        activePattern = pendingPattern;
        patternDirty  = false;
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
    void processBlock (juce::AudioBuffer<float>& buffer, int numChannels,
                       double bpm = 120.0, bool isPlaying = false, double ppqPosition = -1.0)
    {
        // Guard against prepareToPlay not having been called yet.
        if (grainSizeParam == nullptr)
            return;

        const int numSamples = buffer.getNumSamples();
        const int bufLen     = circularBuffer.getNumSamples();

        // Pull any pending pattern update from the UI thread (lock-free: tryEnter).
        if (patternLock.tryEnter())
        {
            if (patternDirty) { activePattern = pendingPattern; patternDirty = false; }
            patternLock.exit();
        }

        const float patternTotalBeats = juce::jmax (0.001f, activePattern.patternBeats);

        const float beatsPerSample = static_cast<float> (bpm) / (60.0f * static_cast<float> (currentSampleRate));

        // Determine how pattern position should advance this block:
        //   - Host playing + PPQ available  → anchor to host timeline
        //   - Host not playing + no audio   → freeze
        //   - Everything else               → free-running accumulator
        const bool hasPpq    = (ppqPosition >= 0.0);
        const bool hasAudio  = buffer.getMagnitude (0, numSamples) > 0.00001f;
        const bool shouldRun = isPlaying || hasAudio;

        if (hasPpq && isPlaying)
        {
            patternBeatPos = std::fmod (static_cast<float> (ppqPosition), patternTotalBeats);
            if (patternBeatPos < 0.0f) patternBeatPos += patternTotalBeats;
        }
        else if (shouldRun && !wasRunning)
        {
            patternBeatPos = 0.0f;
        }

        wasRunning = shouldRun;

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

            // Advance pattern beat position — skipped when frozen (not playing, no audio).
            if (shouldRun)
            {
                patternBeatPos += beatsPerSample;
                if (patternBeatPos >= patternTotalBeats)
                    patternBeatPos = std::fmod (patternBeatPos, patternTotalBeats);
            }

            // Step 2: Check if it's time to spawn a new grain.
            samplesSinceLastGrain += 1.0f;
            if (samplesSinceLastGrain >= samplesPerGrain)
            {
                samplesSinceLastGrain -= samplesPerGrain;

                // ---- Breakpoint envelope pitch selection ----
                // Interpolates in log2 space between the two surrounding control points.
                float selectedPitch = 1.0f;
                {
                    const int n = activePattern.numPoints;
                    if (n == 1)
                    {
                        selectedPitch = activePattern.points[0].pitchRatio;
                    }
                    else if (n > 1)
                    {
                        const float beat = patternBeatPos;
                        int prev = -1;
                        for (int i = 0; i < n; ++i)
                            if (activePattern.points[i].timeBeat <= beat + 0.0001f)
                                prev = i;

                        int   next;
                        float t;
                        if (prev == -1)
                        {
                            prev = n - 1;
                            next = 0;
                            float dt = (patternTotalBeats - activePattern.points[prev].timeBeat)
                                     +  activePattern.points[next].timeBeat;
                            t = (dt > 0.0f) ? (beat + patternTotalBeats - activePattern.points[prev].timeBeat) / dt : 0.0f;
                        }
                        else if (prev == n - 1)
                        {
                            next = 0;
                            float dt = (patternTotalBeats - activePattern.points[prev].timeBeat)
                                     +  activePattern.points[next].timeBeat;
                            t = (dt > 0.0f) ? (beat - activePattern.points[prev].timeBeat) / dt : 0.0f;
                        }
                        else
                        {
                            next = prev + 1;
                            float dt = activePattern.points[next].timeBeat - activePattern.points[prev].timeBeat;
                            t = (dt > 0.0f) ? (beat - activePattern.points[prev].timeBeat) / dt : 0.0f;
                        }

                        t = juce::jlimit (0.0f, 1.0f, t);
                        float logA = std::log2 (juce::jmax (0.001f, activePattern.points[prev].pitchRatio));
                        float logB = std::log2 (juce::jmax (0.001f, activePattern.points[next].pitchRatio));
                        selectedPitch = std::pow (2.0f, logA + t * (logB - logA));
                    }

                    playheadBeat.store (patternBeatPos);
                }

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
    // ---- Old stepped-sequencer APVTS pointers (replaced by SeqPattern) ----
    // std::atomic<float>* seqLengthParam   = nullptr;
    // std::atomic<float>* seqStepParams[8] = {};

    // ---- Pattern sequencer state ----
    mutable juce::CriticalSection patternLock;
    SeqPattern   pendingPattern;             // written by UI thread via setPattern()
    SeqPattern   activePattern;             // read by audio thread only
    bool         patternDirty   = false;    // set by setPattern(), cleared in processBlock
    float        patternBeatPos = 0.0f;     // current beat position within the pattern
    bool         wasRunning     = false;    // tracks audio/play state across blocks for reset-on-start
    std::atomic<float> playheadBeat { 0.0f }; // current beat position, read by UI for playhead

    // int currentStep = 0;  // old stepped-sequencer counter — replaced by patternBeatPos

    juce::AudioBuffer<float> circularBuffer;
    int writePos = 0;
    juce::AudioBuffer<float> wetBuffer;
    std::array<float, HANN_TABLE_SIZE> hannTable;
    std::array<Grain, MAX_GRAINS> grains;
    float samplesSinceLastGrain = 0.0f;
    double currentSampleRate = 44100.0;
    std::mt19937 rng;
};
