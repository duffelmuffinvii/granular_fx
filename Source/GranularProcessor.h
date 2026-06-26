/*
  ==============================================================================
  GranularProcessor.h

  Granular audio effect. Parameters are managed by an AudioProcessorValueTreeState
  (APVTS) that lives in PluginProcessor. Call prepareToPlay() once, then
  processBlock() every audio block.

  PARAMETER IDs:
    "grain_size"        — grain duration in milliseconds      (10–500 ms)
    "grain_density"     — grains started per second           (1–100)
    "position_scatter"  — random variation in start position  (0–1)
    "size_scatter"      — random variation in grain duration  (0–1)
    "pan_scatter"       — random variation in stereo pan      (0–1)
    "dry_wet"           — dry/wet mix                         (0–1)

  FIXED CONSTANTS (require recompile to change):
    BUFFER_DURATION_SECONDS — seconds of audio held in the circular buffer
    MAX_GRAINS              — maximum simultaneous grains
    HANN_TABLE_SIZE         — resolution of the pre-computed window LUT
    MAX_SEQ_STEPS           — maximum breakpoints in the pitch envelope
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


struct Grain
{
    float readPos         = 0.0f;
    float speed           = 1.0f;
    int   durationSamples = 0;
    int   age             = 0;
    float pan             = 0.0f;
    float leftGain        = 1.0f;
    float rightGain       = 0.0f;
    bool  active          = false;
};

class GranularProcessor
{
public:
    GranularProcessor();

    void       setPattern      (const SeqPattern& p);
    SeqPattern getPattern      () const;
    float      getPlayheadBeat () const;

    void prepareToPlay (double sampleRate, int maxBlockSize,
                        juce::AudioProcessorValueTreeState& apvts);

    void processBlock (juce::AudioBuffer<float>& buffer, int numChannels,
                       double bpm = 120.0, bool isPlaying = false, double ppqPosition = -1.0);

private:
    void spawnGrain (int bufLen, float grainSizeMs, float pitchRatio,
                     float positionScatter, float sizeScatter, float panScatter, bool reverse);

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

    mutable juce::CriticalSection patternLock;
    SeqPattern   pendingPattern;
    SeqPattern   activePattern;
    bool         patternDirty   = false;
    float        patternBeatPos = 0.0f;
    std::atomic<float> playheadBeat { 0.0f };

    juce::AudioBuffer<float>           circularBuffer;
    int                                writePos = 0;
    juce::AudioBuffer<float>           wetBuffer;
    std::array<float, HANN_TABLE_SIZE> hannTable;
    std::array<Grain, MAX_GRAINS>      grains;
    float   samplesSinceLastGrain = 0.0f;
    double  currentSampleRate     = 44100.0;
    std::mt19937 rng;
};
