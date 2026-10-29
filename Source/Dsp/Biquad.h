#pragma once

#include <algorithm>
#include <cmath>
#include <numbers>

/// Normalised biquad coefficients (a0 divided out):
///   y[n] = b0 x[n] + b1 x[n-1] + b2 x[n-2] - a1 y[n-1] - a2 y[n-2]
/// The defaults are a pass-through.
struct BiquadCoeffs { float b0 = 1, b1 = 0, b2 = 0, a1 = 0, a2 = 0; };   // a0 normalised to 1

/// Coefficient design from the RBJ "Audio EQ Cookbook". All three are bilinear
/// transforms prewarped at `hz`, so the corner or centre lands exactly where it
/// was asked for at every sample rate. They allocate nothing and take no locks,
/// so smoothed parameters may redesign from the audio thread.
namespace BiquadDesign
{
    namespace detail
    {
        /// Angular frequency of `hz`, kept inside (0, Nyquist) so sin/cos never
        /// degenerate into a division by zero when a caller passes a silly value.
        inline double omega (double sampleRate, double hz) noexcept
        {
            return 2.0 * std::numbers::pi * std::clamp (hz, 1.0, 0.49 * sampleRate) / sampleRate;
        }

        inline BiquadCoeffs normalise (double b0, double b1, double b2,
                                       double a0, double a1, double a2) noexcept
        {
            BiquadCoeffs c;
            c.b0 = (float) (b0 / a0);
            c.b1 = (float) (b1 / a0);
            c.b2 = (float) (b2 / a0);
            c.a1 = (float) (a1 / a0);
            c.a2 = (float) (a2 / a0);
            return c;
        }
    }

    /// 2nd-order high-pass, -3 dB at `hz` when q = 1/sqrt(2).
    inline BiquadCoeffs highPass (double sampleRate, double hz, double q) noexcept
    {
        const double w0 = detail::omega (sampleRate, hz);
        const double cosW = std::cos (w0);
        const double alpha = std::sin (w0) / (2.0 * q);

        return detail::normalise ((1.0 + cosW) / 2.0, -(1.0 + cosW), (1.0 + cosW) / 2.0,
                                  1.0 + alpha, -2.0 * cosW, 1.0 - alpha);
    }

    /// Peaking bell: exactly `gainDb` at `hz`, unity far away from it. At 0 dB the
    /// numerator equals the denominator, so the result is an exact pass-through.
    inline BiquadCoeffs peaking (double sampleRate, double hz, double q, double gainDb) noexcept
    {
        const double w0 = detail::omega (sampleRate, hz);
        const double cosW = std::cos (w0);
        const double alpha = std::sin (w0) / (2.0 * q);
        const double a = std::pow (10.0, gainDb / 40.0);

        return detail::normalise (1.0 + alpha * a, -2.0 * cosW, 1.0 - alpha * a,
                                  1.0 + alpha / a, -2.0 * cosW, 1.0 - alpha / a);
    }

    /// High shelf: unity below `hz`, `gainDb` above it, half the gain in dB at
    /// the corner for slope = 1. A smaller slope gives a gentler transition.
    inline BiquadCoeffs highShelf (double sampleRate, double hz, double slope, double gainDb) noexcept
    {
        const double w0 = detail::omega (sampleRate, hz);
        const double cosW = std::cos (w0);
        const double a = std::pow (10.0, gainDb / 40.0);

        // The radicand goes negative for slopes above 1 with large gains; clamping
        // keeps the design finite instead of producing NaN coefficients.
        const double alpha = std::sin (w0) / 2.0
                           * std::sqrt (std::max (0.0, (a + 1.0 / a) * (1.0 / slope - 1.0) + 2.0));
        const double twoSqrtAAlpha = 2.0 * std::sqrt (a) * alpha;

        return detail::normalise (a * ((a + 1.0) + (a - 1.0) * cosW + twoSqrtAAlpha),
                                  -2.0 * a * ((a - 1.0) + (a + 1.0) * cosW),
                                  a * ((a + 1.0) + (a - 1.0) * cosW - twoSqrtAAlpha),
                                  (a + 1.0) - (a - 1.0) * cosW + twoSqrtAAlpha,
                                  2.0 * ((a - 1.0) - (a + 1.0) * cosW),
                                  (a + 1.0) - (a - 1.0) * cosW - twoSqrtAAlpha);
    }
}

/// Transposed direct form II biquad. The state is deliberately kept across
/// setCoeffs(): smoothed parameters change the coefficients every few samples,
/// and clearing the state each time would click. TDF2 is also the form whose
/// state stays well-behaved when the coefficients move.
///
/// The state is double precision: it costs nothing here and keeps the rounding
/// error of poles that sit close to the unit circle (a 40 Hz high-pass at
/// 96 kHz) far below anything audible.
class Biquad
{
public:
    void setCoeffs (const BiquadCoeffs& c) noexcept
    {
        b0 = c.b0;  b1 = c.b1;  b2 = c.b2;
        a1 = c.a1;  a2 = c.a2;
    }

    void reset() noexcept   { z1 = 0.0; z2 = 0.0; }

    float processSample (float x) noexcept
    {
        const double in = x;
        const double y = b0 * in + z1;

        z1 = b1 * in - a1 * y + z2;
        z2 = b2 * in - a2 * y;

        return (float) y;
    }

private:
    double b0 = 1.0, b1 = 0.0, b2 = 0.0, a1 = 0.0, a2 = 0.0;
    double z1 = 0.0, z2 = 0.0;
};
