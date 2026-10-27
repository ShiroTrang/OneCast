#pragma once

#include <algorithm>
#include <atomic>
#include <cmath>

/// How the noise stage works. Stored as an int in AtomicVoiceParams, so the
/// values are part of the contract: do not renumber them.
enum class NoiseMode    : int { off = 0, light = 1, ai = 2 };

/// Which microphone channel(s) feed the voice chain.
enum class InputChannel : int { first = 0, second = 1, mix = 2 };

struct ParamRange { float min, max, defaultValue; };

/// The single source of truth for every user-facing float parameter: the UI
/// slider limits, the preset validation and clampToRanges() all read these.
namespace ParamRanges
{
    inline constexpr ParamRange preGainDb       { -12.0f, 24.0f,   6.0f };
    inline constexpr ParamRange hpfHz           {  40.0f, 200.0f, 80.0f };
    inline constexpr ParamRange noiseAmount     {   0.0f, 100.0f, 100.0f };  // percent
    inline constexpr ParamRange gateThresholdDb { -80.0f, -20.0f, -50.0f };
    inline constexpr ParamRange eqMudDb         { -12.0f,   0.0f,  -2.0f };  // bell ~250 Hz
    inline constexpr ParamRange eqClarityDb     {   0.0f,   8.0f,   2.0f };  // bell ~3.5 kHz
    inline constexpr ParamRange eqAirDb         {   0.0f,   6.0f,   1.0f };  // shelf ~10 kHz
    inline constexpr ParamRange compThresholdDb { -40.0f,   0.0f, -20.0f };
    inline constexpr ParamRange compRatio       {   1.0f,  10.0f,   3.0f };
    inline constexpr ParamRange compMakeupDb    {   0.0f,  18.0f,   3.0f };
    inline constexpr ParamRange deEssAmount     {   0.0f, 100.0f,  50.0f };  // percent
    inline constexpr ParamRange outputGainDb    { -12.0f,  12.0f,   0.0f };
}

/// Everything the user can change about the voice chain. Plain data so it can
/// be copied between threads, compared in tests and stored in presets.
struct VoiceParams
{
    float preGainDb = 6.0f;
    bool  hpfOn = true;          float hpfHz = 80.0f;
    NoiseMode noiseMode = NoiseMode::ai;  float noiseAmount = 100.0f;
    bool  gateOn = true;         float gateThresholdDb = -50.0f;
    bool  eqOn = true;           float eqMudDb = -2.0f, eqClarityDb = 2.0f, eqAirDb = 1.0f;
    bool  compOn = true;         float compThresholdDb = -20.0f, compRatio = 3.0f, compMakeupDb = 3.0f;
    bool  deEssOn = true;        float deEssAmount = 50.0f;
    float outputGainDb = 0.0f;
    bool  bypass = false;
    bool  mute = false;

    bool operator== (const VoiceParams&) const = default;
};

namespace DspParamsDetail
{
    /// A NaN would slip through std::clamp unchanged and poison every filter
    /// downstream, so it falls back to the default instead.
    inline float clampToRange (float value, const ParamRange& range) noexcept
    {
        if (std::isnan (value))
            return range.defaultValue;

        return std::clamp (value, range.min, range.max);
    }
}

/// Every float clamped to its ParamRanges entry; enums outside their range become the default.
/// Run it on anything that did not come from the UI (presets, settings files) before
/// it reaches the audio thread.
inline VoiceParams clampToRanges (VoiceParams p) noexcept
{
    using DspParamsDetail::clampToRange;

    p.preGainDb       = clampToRange (p.preGainDb,       ParamRanges::preGainDb);
    p.hpfHz           = clampToRange (p.hpfHz,           ParamRanges::hpfHz);
    p.noiseAmount     = clampToRange (p.noiseAmount,     ParamRanges::noiseAmount);
    p.gateThresholdDb = clampToRange (p.gateThresholdDb, ParamRanges::gateThresholdDb);
    p.eqMudDb         = clampToRange (p.eqMudDb,         ParamRanges::eqMudDb);
    p.eqClarityDb     = clampToRange (p.eqClarityDb,     ParamRanges::eqClarityDb);
    p.eqAirDb         = clampToRange (p.eqAirDb,         ParamRanges::eqAirDb);
    p.compThresholdDb = clampToRange (p.compThresholdDb, ParamRanges::compThresholdDb);
    p.compRatio       = clampToRange (p.compRatio,       ParamRanges::compRatio);
    p.compMakeupDb    = clampToRange (p.compMakeupDb,    ParamRanges::compMakeupDb);
    p.deEssAmount     = clampToRange (p.deEssAmount,     ParamRanges::deEssAmount);
    p.outputGainDb    = clampToRange (p.outputGainDb,    ParamRanges::outputGainDb);

    switch (p.noiseMode)
    {
        case NoiseMode::off:
        case NoiseMode::light:
        case NoiseMode::ai:
            break;

        default:
            p.noiseMode = NoiseMode::ai;
            break;
    }

    return p;
}

/// Lock-free hand-over from the message thread to the audio thread: one
/// relaxed atomic per field. A load may mix fields from two stores; every
/// field is independently valid, so that is harmless.
class AtomicVoiceParams
{
public:
    AtomicVoiceParams() noexcept { store (VoiceParams {}); }

    void store (const VoiceParams& p) noexcept
    {
        preGainDb.store       (p.preGainDb,       relaxed);
        hpfOn.store           (p.hpfOn,           relaxed);
        hpfHz.store           (p.hpfHz,           relaxed);
        noiseMode.store       ((int) p.noiseMode, relaxed);
        noiseAmount.store     (p.noiseAmount,     relaxed);
        gateOn.store          (p.gateOn,          relaxed);
        gateThresholdDb.store (p.gateThresholdDb, relaxed);
        eqOn.store            (p.eqOn,            relaxed);
        eqMudDb.store         (p.eqMudDb,         relaxed);
        eqClarityDb.store     (p.eqClarityDb,     relaxed);
        eqAirDb.store         (p.eqAirDb,         relaxed);
        compOn.store          (p.compOn,          relaxed);
        compThresholdDb.store (p.compThresholdDb, relaxed);
        compRatio.store       (p.compRatio,       relaxed);
        compMakeupDb.store    (p.compMakeupDb,    relaxed);
        deEssOn.store         (p.deEssOn,         relaxed);
        deEssAmount.store     (p.deEssAmount,     relaxed);
        outputGainDb.store    (p.outputGainDb,    relaxed);
        bypass.store          (p.bypass,          relaxed);
        mute.store            (p.mute,            relaxed);
    }

    VoiceParams load() const noexcept
    {
        VoiceParams p;
        p.preGainDb       = preGainDb.load       (relaxed);
        p.hpfOn           = hpfOn.load           (relaxed);
        p.hpfHz           = hpfHz.load           (relaxed);
        p.noiseMode       = (NoiseMode) noiseMode.load (relaxed);
        p.noiseAmount     = noiseAmount.load     (relaxed);
        p.gateOn          = gateOn.load          (relaxed);
        p.gateThresholdDb = gateThresholdDb.load (relaxed);
        p.eqOn            = eqOn.load            (relaxed);
        p.eqMudDb         = eqMudDb.load         (relaxed);
        p.eqClarityDb     = eqClarityDb.load     (relaxed);
        p.eqAirDb         = eqAirDb.load         (relaxed);
        p.compOn          = compOn.load          (relaxed);
        p.compThresholdDb = compThresholdDb.load (relaxed);
        p.compRatio       = compRatio.load       (relaxed);
        p.compMakeupDb    = compMakeupDb.load    (relaxed);
        p.deEssOn         = deEssOn.load         (relaxed);
        p.deEssAmount     = deEssAmount.load     (relaxed);
        p.outputGainDb    = outputGainDb.load    (relaxed);
        p.bypass          = bypass.load          (relaxed);
        p.mute            = mute.load            (relaxed);
        return p;
    }

private:
    static constexpr auto relaxed = std::memory_order_relaxed;

    // The audio thread must never block, so every member has to be lock-free.
    static_assert (std::atomic<float>::is_always_lock_free);
    static_assert (std::atomic<int>::is_always_lock_free);
    static_assert (std::atomic<bool>::is_always_lock_free);

    std::atomic<float> preGainDb, hpfHz, noiseAmount, gateThresholdDb,
                       eqMudDb, eqClarityDb, eqAirDb,
                       compThresholdDb, compRatio, compMakeupDb,
                       deEssAmount, outputGainDb;
    std::atomic<int>   noiseMode;
    std::atomic<bool>  hpfOn, gateOn, eqOn, compOn, deEssOn, bypass, mute;
};
