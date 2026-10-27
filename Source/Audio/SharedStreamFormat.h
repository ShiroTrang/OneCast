#pragma once

/// The rate and the sample-rate conversion flag a shared-mode stream needs.
/// Split out of WasapiOutputDevice so the choice can be tested without a
/// device.
struct SharedStreamFormat
{
    /// Rate the stream is opened at. This is also the rate the drift reader
    /// uses as its device rate.
    int rate = 48000;

    /// True when the stream needs WASAPI's AUTOCONVERTPCM: the format is then
    /// at the producer rate and Windows resamples to the endpoint's mix rate.
    bool convert = false;
};

/// Decide how a shared-mode stream is opened when the producer rate may differ
/// from the endpoint's mix rate.
///
/// The low-latency rung (IAudioClient3::InitializeSharedAudioStream) must keep
/// the endpoint's original mix format: that API rejects a format whose rate
/// differs from the mix rate, and it does not accept AUTOCONVERTPCM. The
/// producer/device rate difference is handled by DriftCompensatedReader's
/// nominal ratio instead. The legacy shared rung keeps the old behaviour and
/// lets WASAPI convert.
inline SharedStreamFormat chooseSharedStreamFormat (bool lowLatency,
                                                    int nativeRate,
                                                    int producerRate) noexcept
{
    const bool mismatch = producerRate > 0 && producerRate != nativeRate;

    if (lowLatency)
        return { nativeRate, false };

    return mismatch ? SharedStreamFormat { producerRate, true }
                    : SharedStreamFormat { nativeRate, false };
}
