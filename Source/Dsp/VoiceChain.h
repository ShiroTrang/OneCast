#pragma once
#include <JuceHeader.h>

#include "../Audio/SafetyLimiter.h"
#include "Compressor.h"
#include "DeEsser.h"
#include "DspParams.h"
#include "HighPassFilter.h"
#include "NoiseGate.h"
#include "NoiseSuppressor.h"
#include "VoiceEq.h"

#include <atomic>
#include <vector>

/// The whole voice processing path, assembled in the order the product sounds
/// best in: pre-gain, HPF, noise suppression, gate, EQ, compressor, de-esser,
/// output gain, safety limiter and mute. Bypass skips only HPF..de-esser, so
/// pre-gain, output gain, the limiter and mute still apply to the bypass path.
///
/// Every audible parameter is smoothed on the audio thread, indexed by the
/// sample count rather than block boundaries, so the output does not depend on
/// how the host chops the stream into blocks. Module on/off switches crossfade
/// the module input and output; bypass crossfades the full path against the
/// bypass path; mute is a short fade to silence. prepare() allocates; process()
/// does not.
class VoiceChain
{
public:
    static constexpr double toggleFadeSeconds = 0.01;   // module on/off and mute
    static constexpr double bypassFadeSeconds = 0.02;

    void prepare (double sampleRate, int maxBlockSize);
    void reset() noexcept;

    /// Mono, in place, audio thread. Non-finite input samples become 0 before
    /// anything else. `params` is a snapshot: all setters are called from here.
    void process (float* mono, int numSamples, const VoiceParams& params) noexcept;

    /// What is heard now, limiter included.
    int getLatencySamples() const noexcept;

    struct Levels { float inputDb, outputDb, gateDb, compDb, deEssDb, limiterDb; };

    /// Safe to read from any thread.
    Levels getLevels() const noexcept;

private:
    using Smoother = juce::SmoothedValue<float, juce::ValueSmoothingTypes::Linear>;

    struct Toggle
    {
        float mix = 1.0f;    // 0 = dry/bypassed, 1 = wet/active
        float target = 1.0f;
        bool snap = true;

        void setTarget (bool on) noexcept
        {
            target = on ? 1.0f : 0.0f;

            if (snap)
            {
                mix = target;
                snap = false;
            }
        }

        void advance (float step) noexcept
        {
            if (mix == target)
                return;

            if (mix < target)
                mix = juce::jmin (target, mix + step);
            else
                mix = juce::jmax (target, mix - step);
        }
    };

    void updateParameters (const VoiceParams& params) noexcept;
    void setGainTarget (Smoother& smoother, bool& primed, float db, const ParamRange& range) noexcept;
    void processChunk (float* mono, int numSamples) noexcept;

    template <typename Module>
    void processModule (Module& module, float* io, int numSamples, Toggle& toggle, float step) noexcept
    {
        // `io` is the dry signal; the module runs on a copy, so both sides of
        // the crossfade are available for every sample.
        std::copy (io, io + numSamples, scratchA_.begin());
        module.process (scratchA_.data(), numSamples);

        for (int i = 0; i < numSamples; ++i)
        {
            toggle.advance (step);
            io[i] += toggle.mix * (scratchA_[i] - io[i]);
        }
    }

    static constexpr double gainSmoothingSeconds = 0.02;
    static constexpr double peakReleaseSeconds = 0.15;
    static constexpr float silenceDb = -200.0f;

    HighPassFilter hpf_;
    NoiseSuppressor noise_;
    NoiseGate gate_;
    VoiceEq eq_;
    Compressor comp_;
    DeEsser deEss_;
    SafetyLimiter limiter_;

    Smoother preGainDb_   { ParamRanges::preGainDb.defaultValue };
    Smoother outputGainDb_{ ParamRanges::outputGainDb.defaultValue };
    bool preGainPrimed_ = false;
    bool outputGainPrimed_ = false;

    Toggle hpfToggle_, gateToggle_, eqToggle_, compToggle_, deEssToggle_;
    Toggle bypassToggle_, muteToggle_;

    double sampleRate_ = 48000.0;
    int maxBlockSize_ = 0;
    float toggleStep_ = 1.0f;
    float bypassStep_ = 1.0f;
    float muteStep_ = 1.0f;
    float peakRelease_ = 0.0f;

    std::vector<float> scratchA_;
    std::vector<float> scratchB_;

    float inputPeak_ = 0.0f;
    float outputPeak_ = 0.0f;

    // Written after every block, so a reader on another thread sees "what is
    // heard now" without touching the audio-thread-only stage state.
    std::atomic<int> latencySamples_ { 0 };
    std::atomic<float> inputDb_  { silenceDb };
    std::atomic<float> outputDb_ { silenceDb };
};
