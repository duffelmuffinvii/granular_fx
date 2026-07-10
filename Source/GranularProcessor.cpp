#include "GranularProcessor.h"

GranularProcessor::GranularProcessor() : rng (std::random_device{}()) {}

void GranularProcessor::setPattern (const SeqPattern& p)
{
    juce::ScopedLock sl (patternLock);
    pendingPattern = p;
    patternDirty   = true;
}

SeqPattern GranularProcessor::getPattern() const
{
    juce::ScopedLock sl (patternLock);
    return pendingPattern;
}

float GranularProcessor::getPlayheadBeat() const
{
    return playheadBeat.load();
}

void GranularProcessor::prepareToPlay (double sampleRate, int maxBlockSize,
                                       juce::AudioProcessorValueTreeState& apvts)
{
    currentSampleRate = sampleRate;

    grainSizeParam       = apvts.getRawParameterValue ("grain_size");
    grainDensityParam    = apvts.getRawParameterValue ("grain_density");
    positionScatterParam = apvts.getRawParameterValue ("position_scatter");
    sizeScatterParam     = apvts.getRawParameterValue ("size_scatter");
    panScatterParam      = apvts.getRawParameterValue ("pan_scatter");
    dryWetParam          = apvts.getRawParameterValue ("dry_wet");
    reverseParam         = apvts.getRawParameterValue ("reverse");
    densitySyncParam     = apvts.getRawParameterValue ("density_sync");
    sizeSyncParam        = apvts.getRawParameterValue ("size_sync");
    densityDivisionParam = apvts.getRawParameterValue ("density_division");
    sizeDivisionParam    = apvts.getRawParameterValue ("size_division");

    // ---- RHYTHM FX (experimental — see maybeSpawnGrain() to remove) ----
    spawnProbabilityParam = apvts.getRawParameterValue ("spawn_probability");
    swingParam             = apvts.getRawParameterValue ("swing");
    // ---- end RHYTHM FX ----

    int bufLenSamples = static_cast<int> (sampleRate * BUFFER_DURATION_SECONDS);
    circularBuffer.setSize (2, bufLenSamples);
    circularBuffer.clear();
    writePos = 0;

    wetBuffer.setSize (2, maxBlockSize);
    wetBuffer.clear();

    for (int i = 0; i < HANN_TABLE_SIZE; ++i)
        hannTable[i] = 0.5f * (1.0f - std::cos (
            juce::MathConstants<float>::twoPi * i / HANN_TABLE_SIZE));

    for (auto& g : grains)
        g.active = false;

    samplesSinceLastGrain = 0.0f;
    patternBeatPos = 0.0f;
    playheadBeat.store (0.0f);
    grainParity = false;  // RHYTHM FX

    activePattern = pendingPattern;
    patternDirty  = false;
}

void GranularProcessor::processBlock (juce::AudioBuffer<float>& buffer, int numChannels,
                                      double bpm, bool isPlaying, double ppqPosition)
{
    if (grainSizeParam == nullptr)
        return;

    const int numSamples = buffer.getNumSamples();
    const int bufLen     = circularBuffer.getNumSamples();

    if (patternLock.tryEnter())
    {
        if (patternDirty) { activePattern = pendingPattern; patternDirty = false; }
        patternLock.exit();
    }

    const float patternTotalBeats = juce::jmax (0.001f, activePattern.patternBeats);
    const float beatsPerSample    = static_cast<float> (bpm) / (60.0f * static_cast<float> (currentSampleRate));
    const bool  hasPpq            = (ppqPosition >= 0.0);

    if (isPlaying && hasPpq)
    {
        patternBeatPos = std::fmod (static_cast<float> (ppqPosition), patternTotalBeats);
        if (patternBeatPos < 0.0f) patternBeatPos += patternTotalBeats;
    }

    const BlockParams bp = resolveBlockParams (bpm);

    wetBuffer.clear();

    for (int s = 0; s < numSamples; ++s)
    {
        for (int ch = 0; ch < juce::jmin (numChannels, 2); ++ch)
            circularBuffer.setSample (ch, writePos, buffer.getSample (ch, s));

        writePos = (writePos + 1) % bufLen;

        if (isPlaying)
        {
            patternBeatPos += beatsPerSample;
            if (patternBeatPos >= patternTotalBeats)
                patternBeatPos = std::fmod (patternBeatPos, patternTotalBeats);
        }

        maybeSpawnGrain (bufLen, bp);

        for (auto& grain : grains)
        {
            if (!grain.active)
                continue;

            float phase    = static_cast<float> (grain.age) / static_cast<float> (grain.durationSamples);
            int   tableIdx = static_cast<int> (phase * (HANN_TABLE_SIZE - 1));
            float window   = hannTable[tableIdx];

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

    for (int s = 0; s < numSamples; ++s)
    {
        for (int ch = 0; ch < numChannels; ++ch)
        {
            float dry = buffer.getSample (ch, s);
            float wet = wetBuffer.getSample (ch, s);
            buffer.setSample (ch, s, dry * (1.0f - bp.dryWet) + wet * bp.wetGain * bp.dryWet);
        }
    }
}

// Looks up the pitch ratio at a given position in the active pattern,
// linearly interpolating between the surrounding breakpoints in log2
// (musical) space. Wraps around the pattern end back to the first point.
float GranularProcessor::getPitchAtBeat (float beat) const
{
    const int n = activePattern.numPoints;
    if (n == 0)
        return 1.0f;
    if (n == 1)
        return activePattern.points[0].pitchRatio;

    const float patternTotalBeats = juce::jmax (0.001f, activePattern.patternBeats);

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
    return std::pow (2.0f, logA + t * (logB - logA));
}

// Reads the current parameter atomics into a single BlockParams snapshot,
// resolving tempo-sync (grain size / density locked to a musical division)
// into plain values so the per-sample loop never has to branch on sync mode.
BlockParams GranularProcessor::resolveBlockParams (double bpm) const
{
    const bool densitySync = densitySyncParam->load() >= 0.5f;
    const bool sizeSync    = sizeSyncParam->load()    >= 0.5f;

    float grainSizeMs;
    if (sizeSync)
    {
        int idx     = juce::jlimit (0, kNumTempoDivisions - 1, static_cast<int> (sizeDivisionParam->load()));
        grainSizeMs = kTempoDivisions[idx].beatFraction * 60000.0f / static_cast<float> (bpm);
        grainSizeMs = juce::jlimit (10.0f, 500.0f, grainSizeMs);
    }
    else
    {
        grainSizeMs = grainSizeParam->load();
    }

    float grainDensity;
    if (densitySync)
    {
        int idx      = juce::jlimit (0, kNumTempoDivisions - 1, static_cast<int> (densityDivisionParam->load()));
        grainDensity = static_cast<float> (bpm) / (kTempoDivisions[idx].beatFraction * 60.0f);
        grainDensity = juce::jlimit (1.0f, 100.0f, grainDensity);
    }
    else
    {
        grainDensity = grainDensityParam->load();
    }

    BlockParams bp;
    bp.grainSizeMs     = grainSizeMs;
    bp.reverse         = reverseParam->load() >= 0.5f;
    bp.positionScatter = positionScatterParam->load();
    bp.sizeScatter     = sizeScatterParam->load();
    bp.panScatter      = panScatterParam->load();
    bp.dryWet          = dryWetParam->load();

    bp.samplesPerGrain = static_cast<float> (currentSampleRate) / grainDensity;
    const float expectedActive = grainDensity * (grainSizeMs / 1000.0f) * (1.0f + bp.sizeScatter);
    bp.wetGain = 1.0f / juce::jmax (1.0f, expectedActive);

    // ---- RHYTHM FX (experimental — see maybeSpawnGrain() to remove) ----
    bp.spawnProbability = spawnProbabilityParam->load();
    bp.swingAmount      = swingParam->load();
    // ---- end RHYTHM FX ----

    return bp;
}

// Called once per sample. Advances the grain-spawn timer; when a grain is
// due, resolves the current pitch from the envelope, publishes the
// playhead position for the UI, and asks spawnGrain to claim a free voice.
//
// RHYTHM FX (experimental): swing delays every other spawn slot by a
// fraction of the interval, and spawn probability can skip a due grain
// for sparser rhythms. Both default to a no-op (swing=0, probability=1),
// so behaviour is unchanged unless those params are moved off default.
// To remove this feature: delete the two marked blocks below and restore
// the unconditional spawn (see git history for the prior version).
void GranularProcessor::maybeSpawnGrain (int bufLen, const BlockParams& bp)
{
    samplesSinceLastGrain += 1.0f;

    const float swingOffset = grainParity ? bp.swingAmount * bp.samplesPerGrain : 0.0f;
    if (samplesSinceLastGrain < bp.samplesPerGrain + swingOffset)
        return;

    samplesSinceLastGrain -= bp.samplesPerGrain + swingOffset;
    grainParity = ! grainParity;

    playheadBeat.store (patternBeatPos);

    if (bp.spawnProbability < 1.0f && randF() > bp.spawnProbability)
        return;

    const float selectedPitch = getPitchAtBeat (patternBeatPos);
    spawnGrain (bufLen, { bp.grainSizeMs, selectedPitch, bp.positionScatter, bp.sizeScatter, bp.panScatter, bp.reverse });
}

void GranularProcessor::spawnGrain (int bufLen, const SpawnParams& p)
{
    Grain* slot = nullptr;
    for (auto& g : grains)
    {
        if (!g.active) { slot = &g; break; }
    }
    if (slot == nullptr)
        return;

    float baseDuration = (p.grainSizeMs / 1000.0f) * static_cast<float> (currentSampleRate);

    slot->durationSamples = juce::jmax (1, static_cast<int> (
        baseDuration * (1.0f - p.sizeScatter + 2.0f * p.sizeScatter * randF())));

    int lookback = static_cast<int> (baseDuration + randF() * p.positionScatter * bufLen);
    lookback = juce::jmin (lookback, bufLen - 1);
    slot->readPos = static_cast<float> ((writePos - lookback + bufLen) % bufLen);

    slot->speed = p.reverse ? -p.pitchRatio : p.pitchRatio;

    slot->pan = -p.panScatter + 2.0f * p.panScatter * randF();

    float panAngle  = (slot->pan + 1.0f) * 0.25f * juce::MathConstants<float>::pi;
    slot->leftGain  = std::cos (panAngle);
    slot->rightGain = std::sin (panAngle);

    slot->age    = 0;
    slot->active = true;
}
