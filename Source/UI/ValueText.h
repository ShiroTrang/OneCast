#pragma once
#include <JuceHeader.h>

#include "../Dsp/DspParams.h"

#include <cmath>
#include <optional>

/** Formatting and parsing for the numeric readout under a knob.

    The readout is the only place a value can be typed, so the format and the
    parser have to agree exactly: whatever is printed has to be accepted again,
    and every other accepted form has to end up at the number the user meant.
    Keeping this out of ModuleCard makes it testable without a window.
*/
namespace ValueText
{
    /// The four units the chain uses. The kind, not the unit string, decides how
    /// a value is printed and what may follow the number when typed.
    enum class Kind { decibels, hertz, percent, ratio };

    inline Kind kindForUnit (const juce::String& unit)
    {
        if (unit == "Hz")
            return Kind::hertz;

        if (unit == "%")
            return Kind::percent;

        if (unit.startsWith (":"))
            return Kind::ratio;

        return Kind::decibels;
    }

    /// Integer hertz and percent, one decimal for dB and the ratio, an explicit
    /// sign for positive gains and a plain "0.0 dB" so zero never reads as +0.0.
    inline juce::String format (float value, Kind kind)
    {
        if (! std::isfinite (value))
            value = 0.0f;

        switch (kind)
        {
            case Kind::hertz:
                return juce::String (juce::roundToInt (value)) + " Hz";

            case Kind::percent:
                return juce::String (juce::roundToInt (value)) + " %";

            case Kind::ratio:
                return juce::String (value, 1) + ":1";

            case Kind::decibels:
            default:
                break;
        }

        const float rounded = std::round (value * 10.0f) / 10.0f;

        // -0.0 would print as "-0.0"; a reading that rounds to zero is zero.
        if (std::abs (rounded) < 0.05f)
            return "0.0 dB";

        const auto number = juce::String (rounded, 1);
        return (rounded > 0.0f ? "+" : "") + number + " dB";
    }

    namespace detail
    {
        /// Spaces are optional everywhere ("6 dB", "6db", " 6 "), so they are
        /// removed before the number and its optional unit are inspected.
        inline juce::String withoutSpaces (const juce::String& text)
        {
            juce::String out;

            for (auto c : text)
                if (! juce::CharacterFunctions::isWhitespace (c))
                    out += c;

            return out;
        }

        /// A number with an optional sign and at most one decimal point. This is
        /// stricter than String::getDoubleValue, which happily stops at the first
        /// junk character and would turn "6 xyz" into 6.
        inline bool isStrictNumber (juce::String text)
        {
            if (text.isEmpty())
                return false;

            if (text[0] == '+' || text[0] == '-')
                text = text.substring (1);

            bool hasDigit = false;
            bool hasPoint = false;

            for (auto i = 0; i < text.length(); ++i)
            {
                const auto c = text[i];

                if (juce::CharacterFunctions::isDigit (c))
                {
                    hasDigit = true;
                    continue;
                }

                if (c == '.' && ! hasPoint)
                {
                    hasPoint = true;
                    continue;
                }

                return false;
            }

            return hasDigit;
        }
    }

    /** Parses a typed readout. The unit may be omitted or follow the number;
        a comma decimal separator is accepted. Nullopt means "not a number for
        this knob", which leaves the current value alone. Valid numbers are
        clamped to the range the knob was built with.
    */
    inline std::optional<float> parse (const juce::String& text, Kind kind,
                                       const ParamRange& range)
    {
        auto cleaned = detail::withoutSpaces (text);

        if (cleaned.isEmpty())
            return std::nullopt;

        cleaned = cleaned.replaceCharacter (',', '.');

        // Strip exactly this kind's unit. Any other trailing text fails below,
        // so "6 Hz" is not silently taken as a dB value.
        switch (kind)
        {
            case Kind::hertz:
                if (cleaned.endsWithIgnoreCase ("hz"))
                    cleaned = cleaned.dropLastCharacters (2);
                break;

            case Kind::percent:
                if (cleaned.endsWithChar ('%'))
                    cleaned = cleaned.dropLastCharacters (1);
                break;

            case Kind::ratio:
                if (cleaned.endsWith (":1"))
                    cleaned = cleaned.dropLastCharacters (2);
                else if (cleaned.containsChar (':'))
                    return std::nullopt;
                break;

            case Kind::decibels:
            default:
                if (cleaned.endsWithIgnoreCase ("db"))
                    cleaned = cleaned.dropLastCharacters (2);
                break;
        }

        if (! detail::isStrictNumber (cleaned))
            return std::nullopt;

        const auto value = (float) cleaned.getDoubleValue();

        if (! std::isfinite (value))
            return std::nullopt;

        return juce::jlimit (range.min, range.max, value);
    }
}
