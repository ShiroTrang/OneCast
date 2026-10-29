#pragma once
#include <algorithm>

/// The last clamp on the render path, after every gain stage and the
/// resampler.
///
/// The voice chain's limiter keeps the signal under -1 dBFS, but it runs
/// before DriftCompensatedReader's 4-point Lagrange interpolation. Interpolation
/// of a signal that already sits at the ceiling overshoots (a step or random
/// samples between two high samples can reach about +0.9 dBFS), and the
/// interleave and PCM conversions after it do not clamp. OneCast promises that
/// no sample leaves the app above -1 dBFS, so the render loop clamps right
/// where it writes the endpoint buffer, for float and integer formats alike.
namespace OutputCeiling
{
    /// -1 dBFS, matching SafetyLimiter's ceiling. The last digit is kept as the
    /// float nearest to 10^(-1/20); rounding it up (0.89125091) would exceed
    /// the promise by one ulp.
    constexpr float ceiling = 0.8912509f;

    /// Symmetric clamp. NaN is not expected here (the chain sanitises), but
    /// std::min/std::max order NaN to the second argument, so a NaN becomes
    /// +ceiling instead of propagating into the endpoint buffer.
    inline float clamp (float sample) noexcept
    {
        return std::min (ceiling, std::max (-ceiling, sample));
    }
}
