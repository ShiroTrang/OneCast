#include "VoiceChain.h"

#include <algorithm>
#include <cmath>

void VoiceChain::prepare (double sampleRate, int maxBlockSize)
{
    jassert (sampleRate > 0.0 && maxBlockSize > 0);

    sampleRate_ = sampleRate > 0.0 ? sampleRate : 48000.0;
    maxBlockSize_ = std::max (1, maxBlockSize);

    scratchA_.assign ((size_t) maxBlockSize_, 0.0f);
    scratchB_.assign ((size_t) maxBlockSize_, 0.0f);

    hpf_.prepare (sampleRate_);
    noise_.prepare (sampleRate_, maxBlockSize_);
    gate_.prepare (sampleRate_);
    eq_.prepare (sampleRate_);
    comp_.prepare (sampleRate_);
    deEss_.prepare (sampleRate_);
    limiter_.prepare (1, sampleRate_);

    preGainDb_.reset (sampleRate_, gainSmoothingSeconds);
    outputGainDb_.reset (sampleRate_, gainSmoothingSeconds);

    toggleStep_ = (float) (1.0 / std::max (1.0, toggleFadeSeconds * sampleRate_));
    bypassStep_ = (float) (1.0 / std::max (1.0, bypassFadeSeconds * sampleRate_));
    muteStep_ = toggleStep_;
    peakRelease_ = (float) std::exp (-1.0 / (peakReleaseSeconds * sampleRate_));

    reset();
}

void VoiceChain::reset() noexcept
{
    hpf_.reset();
    noise_.reset();
    gate_.reset();
    eq_.reset();
    comp_.reset();
    deEss_.reset();
    limiter_.reset();

    preGainDb_.setCurrentAndTargetValue (ParamRanges::preGainDb.defaultValue);
    outputGainDb_.setCurrentAndTargetValue (ParamRanges::outputGainDb.defaultValue);
    preGainPrimed_ = false;
    outputGainPrimed_ = false;

    // The first process() after reset() snaps every toggle to the snapshot it
    // is given; these defaults only matter before that first block.
    hpfToggle_ = Toggle {};
    gateToggle_ = Toggle {};
    eqToggle_ = Toggle {};
    compToggle_ = Toggle {};
    deEssToggle_ = Toggle {};

    bypassToggle_ = Toggle {};
    bypassToggle_.mix = 0.0f;
    bypassToggle_.target = 0.0f;

    muteToggle_ = Toggle {};
    muteToggle_.mix = 1.0f;
    muteToggle_.target = 1.0f;

    inputPeak_ = 0.0f;
    outputPeak_ = 0.0f;
    latencySamples_.store (limiter_.getLatencySamples(), std::memory_order_relaxed);
    inputDb_.store (silenceDb, std::memory_order_relaxed);
    outputDb_.store (silenceDb, std::memory_order_relaxed);
}

void VoiceChain::process (float* mono, int numSamples, const VoiceParams& params) noexcept
{
    if (mono == nullptr || numSamples <= 0 || maxBlockSize_ <= 0)
        return;

    juce::ScopedNoDenormals noDenormals;

    updateParameters (params);

    int done = 0;

    while (done < numSamples)
    {
        const int n = std::min (numSamples - done, maxBlockSize_);
        processChunk (mono + done, n);
        done += n;
    }

    inputDb_.store (juce::Decibels::gainToDecibels (inputPeak_, silenceDb), std::memory_order_relaxed);
    outputDb_.store (juce::Decibels::gainToDecibels (outputPeak_, silenceDb), std::memory_order_relaxed);

    // A fully bypassed chain plays the post-pre-gain signal, so the noise
    // stage's delay is no longer heard. During the crossfade both paths exist
    // and the processed one (the longer delay) is reported.
    const int heardNoiseLatency = bypassToggle_.mix >= 1.0f ? 0 : noise_.getLatencySamples();
    latencySamples_.store (heardNoiseLatency + limiter_.getLatencySamples(),
                           std::memory_order_relaxed);
}

int VoiceChain::getLatencySamples() const noexcept
{
    return latencySamples_.load (std::memory_order_relaxed);
}

VoiceChain::Levels VoiceChain::getLevels() const noexcept
{
    Levels levels;
    levels.inputDb = inputDb_.load (std::memory_order_relaxed);
    levels.outputDb = outputDb_.load (std::memory_order_relaxed);
    levels.gateDb = gate_.getGainReductionDb();
    levels.compDb = comp_.getGainReductionDb();
    levels.deEssDb = deEss_.getGainReductionDb();
    levels.limiterDb = limiter_.getGainReductionDb();
    return levels;
}

void VoiceChain::updateParameters (const VoiceParams& params) noexcept
{
    setGainTarget (preGainDb_, preGainPrimed_, params.preGainDb, ParamRanges::preGainDb);
    setGainTarget (outputGainDb_, outputGainPrimed_, params.outputGainDb, ParamRanges::outputGainDb);

    hpf_.setCutoffHz (params.hpfHz);

    noise_.setMode (params.noiseMode);
    noise_.setAmount (params.noiseAmount);

    gate_.setThresholdDb (params.gateThresholdDb);
    eq_.setGains (params.eqMudDb, params.eqClarityDb, params.eqAirDb);
    comp_.setParams (params.compThresholdDb, params.compRatio, params.compMakeupDb);
    deEss_.setAmount (params.deEssAmount);

    hpfToggle_.setTarget (params.hpfOn);
    gateToggle_.setTarget (params.gateOn);
    eqToggle_.setTarget (params.eqOn);
    compToggle_.setTarget (params.compOn);
    deEssToggle_.setTarget (params.deEssOn);
    bypassToggle_.setTarget (params.bypass);
    muteToggle_.setTarget (! params.mute);
}

void VoiceChain::setGainTarget (Smoother& smoother, bool& primed, float db, const ParamRange& range) noexcept
{
    const float clamped = DspParamsDetail::clampToRange (db, range);

    if (primed)
        smoother.setTargetValue (clamped);
    else
    {
        smoother.setCurrentAndTargetValue (clamped);
        primed = true;
    }
}

void VoiceChain::processChunk (float* mono, int numSamples) noexcept
{
    // Non-finite samples must be replaced before anything else: one NaN would
    // stay in every recursive filter state downstream forever.
    for (int i = 0; i < numSamples; ++i)
    {
        float x = mono[i];

        if (! std::isfinite (x))
            x = 0.0f;

        const float preGain = juce::Decibels::decibelsToGain (preGainDb_.getNextValue());
        x *= preGain;

        mono[i] = x;
        scratchB_[i] = x;   // the bypass path starts after pre-gain

        inputPeak_ = juce::jmax (std::abs (x), inputPeak_ * peakRelease_);
    }

    processModule (hpf_, mono, numSamples, hpfToggle_, toggleStep_);
    noise_.process (mono, numSamples);
    processModule (gate_, mono, numSamples, gateToggle_, toggleStep_);
    processModule (eq_, mono, numSamples, eqToggle_, toggleStep_);
    processModule (comp_, mono, numSamples, compToggle_, toggleStep_);
    processModule (deEss_, mono, numSamples, deEssToggle_, toggleStep_);

    // Bypass crosses the full path against the pre-gained dry path. Output gain
    // and the limiter stay common so bypass really skips only HPF..de-esser.
    for (int i = 0; i < numSamples; ++i)
    {
        bypassToggle_.advance (bypassStep_);
        mono[i] += bypassToggle_.mix * (scratchB_[i] - mono[i]);
    }

    for (int i = 0; i < numSamples; ++i)
    {
        const float outputGain = juce::Decibels::decibelsToGain (outputGainDb_.getNextValue());
        mono[i] *= outputGain;
    }

    float* channelData[1] = { mono };
    juce::AudioBuffer<float> limiterBuffer (channelData, 1, numSamples);
    limiter_.process (limiterBuffer, numSamples);

    for (int i = 0; i < numSamples; ++i)
    {
        muteToggle_.advance (muteStep_);
        mono[i] *= muteToggle_.mix;
        outputPeak_ = juce::jmax (std::abs (mono[i]), outputPeak_ * peakRelease_);
    }
}
