// Unit tests for the UI helpers: the knob readout (the text a knob prints and
// the text a user may type into it) and the rule that decides whether the
// options callout opens with its invalid-shortcut warning. Both helpers are
// pure, so no window or audio device is needed.
//
//   cmake --build build --config Release --target OneCastTests
//   build\OneCastTests_artefacts\Release\OneCastTests.exe ui

#include <JuceHeader.h>

#include "../Source/UI/HotkeyStatus.h"
#include "../Source/UI/ValueText.h"

#include <optional>

namespace
{
    using Kind = ValueText::Kind;

    void expectParsed (juce::UnitTest& test, const juce::String& text, Kind kind,
                       const ParamRange& range, float expected)
    {
        const auto parsed = ValueText::parse (text, kind, range);

        test.expect (parsed.has_value(), "\"" + text + "\" should parse");

        if (parsed.has_value())
            test.expectWithinAbsoluteError (*parsed, expected, 1.0e-4f,
                                            "\"" + text + "\" parsed wrong");
    }

    void expectRejected (juce::UnitTest& test, const juce::String& text, Kind kind,
                         const ParamRange& range)
    {
        test.expect (! ValueText::parse (text, kind, range).has_value(),
                     "\"" + text + "\" should not parse");
    }
}

class HotkeyStatusTests final : public juce::UnitTest
{
public:
    HotkeyStatusTests() : juce::UnitTest ("Hotkey notice", "ui") {}

    void runTest() override
    {
        beginTest ("invalid-shortcut notice follows saved text and registration");
        {
            // No saved shortcut at all is not a failure: a new installation
            // must not open with a red warning.
            expect (! HotkeyStatus::invalidNoticeVisible (false, false),
                    "no saved hotkey must not warn");
            expect (! HotkeyStatus::invalidNoticeVisible (false, true),
                    "no saved hotkey must not warn even if a shortcut is held");

            // Saved text that Windows did not take (invalid, or owned by
            // another application) is exactly the case the label explains.
            expect (HotkeyStatus::invalidNoticeVisible (true, false),
                    "unregistered saved hotkey must warn");

            // The normal launch: the saved text is the shortcut really held.
            expect (! HotkeyStatus::invalidNoticeVisible (true, true),
                    "registered saved hotkey must not warn");
        }
    }
};

class ValueTextTests final : public juce::UnitTest
{
public:
    ValueTextTests() : juce::UnitTest ("Knob value text", "ui") {}

    void runTest() override
    {
        beginTest ("format: dB gains carry an explicit sign, zero does not");
        {
            expectEquals (ValueText::format (6.0f, Kind::decibels),
                          juce::String ("+6.0 dB"));
            expectEquals (ValueText::format (-20.0f, Kind::decibels),
                          juce::String ("-20.0 dB"));
            expectEquals (ValueText::format (0.0f, Kind::decibels),
                          juce::String ("0.0 dB"));
            expectEquals (ValueText::format (-0.04f, Kind::decibels),
                          juce::String ("0.0 dB"));
            expectEquals (ValueText::format (2.96f, Kind::decibels),
                          juce::String ("+3.0 dB"));
        }

        beginTest ("format: hertz and percent are whole numbers, ratio has one decimal");
        {
            expectEquals (ValueText::format (80.0f, Kind::hertz),
                          juce::String ("80 Hz"));
            expectEquals (ValueText::format (100.0f, Kind::percent),
                          juce::String ("100 %"));
            expectEquals (ValueText::format (3.0f, Kind::ratio),
                          juce::String ("3.0:1"));
            expectEquals (ValueText::format (2.5f, Kind::ratio),
                          juce::String ("2.5:1"));
        }

        beginTest ("parse: a bare number and the printed form both work");
        {
            expectParsed (*this, "6", Kind::decibels, ParamRanges::preGainDb, 6.0f);
            expectParsed (*this, "+6", Kind::decibels, ParamRanges::preGainDb, 6.0f);
            expectParsed (*this, "6 dB", Kind::decibels, ParamRanges::preGainDb, 6.0f);
            expectParsed (*this, "6dB", Kind::decibels, ParamRanges::preGainDb, 6.0f);
            expectParsed (*this, "-20db", Kind::decibels, ParamRanges::gateThresholdDb, -20.0f);
            expectParsed (*this, "  +6.0 dB  ", Kind::decibels, ParamRanges::preGainDb, 6.0f);

            expectParsed (*this, "80", Kind::hertz, ParamRanges::hpfHz, 80.0f);
            expectParsed (*this, "80 hz", Kind::hertz, ParamRanges::hpfHz, 80.0f);
            expectParsed (*this, "80 HZ", Kind::hertz, ParamRanges::hpfHz, 80.0f);

            expectParsed (*this, "100%", Kind::percent, ParamRanges::noiseAmount, 100.0f);
            expectParsed (*this, "100 %", Kind::percent, ParamRanges::noiseAmount, 100.0f);

            expectParsed (*this, "3", Kind::ratio, ParamRanges::compRatio, 3.0f);
            expectParsed (*this, "3:1", Kind::ratio, ParamRanges::compRatio, 3.0f);
            expectParsed (*this, "3.0 : 1", Kind::ratio, ParamRanges::compRatio, 3.0f);
        }

        beginTest ("parse: a comma decimal separator is accepted");
        {
            expectParsed (*this, "2,5", Kind::decibels, ParamRanges::preGainDb, 2.5f);
            expectParsed (*this, "-2,5 dB", Kind::decibels, ParamRanges::preGainDb, -2.5f);
            expectParsed (*this, "2,5:1", Kind::ratio, ParamRanges::compRatio, 2.5f);
        }

        beginTest ("parse: out-of-range input is clamped to the parameter range");
        {
            expectParsed (*this, "999", Kind::decibels, ParamRanges::preGainDb,
                          ParamRanges::preGainDb.max);
            expectParsed (*this, "-999", Kind::decibels, ParamRanges::preGainDb,
                          ParamRanges::preGainDb.min);
            expectParsed (*this, "-200 dB", Kind::decibels, ParamRanges::gateThresholdDb,
                          ParamRanges::gateThresholdDb.min);
            expectParsed (*this, "10", Kind::hertz, ParamRanges::hpfHz,
                          ParamRanges::hpfHz.min);
            expectParsed (*this, "500 Hz", Kind::hertz, ParamRanges::hpfHz,
                          ParamRanges::hpfHz.max);
            expectParsed (*this, "150 %", Kind::percent, ParamRanges::noiseAmount,
                          ParamRanges::noiseAmount.max);
            expectParsed (*this, "-5", Kind::percent, ParamRanges::noiseAmount,
                          ParamRanges::noiseAmount.min);
            expectParsed (*this, "0.5", Kind::ratio, ParamRanges::compRatio,
                          ParamRanges::compRatio.min);
            expectParsed (*this, "20:1", Kind::ratio, ParamRanges::compRatio,
                          ParamRanges::compRatio.max);
        }

        beginTest ("parse: garbage, a missing number and the wrong unit are rejected");
        {
            expectRejected (*this, "", Kind::decibels, ParamRanges::preGainDb);
            expectRejected (*this, "   ", Kind::decibels, ParamRanges::preGainDb);
            expectRejected (*this, "abc", Kind::decibels, ParamRanges::preGainDb);
            expectRejected (*this, "db", Kind::decibels, ParamRanges::preGainDb);
            expectRejected (*this, "+", Kind::decibels, ParamRanges::preGainDb);
            expectRejected (*this, "--6", Kind::decibels, ParamRanges::preGainDb);
            expectRejected (*this, "6.6.6", Kind::decibels, ParamRanges::preGainDb);
            expectRejected (*this, "6 xyz", Kind::decibels, ParamRanges::preGainDb);
            expectRejected (*this, "6 hz", Kind::decibels, ParamRanges::preGainDb);
            expectRejected (*this, "1e3", Kind::decibels, ParamRanges::preGainDb);

            expectRejected (*this, "80 db", Kind::hertz, ParamRanges::hpfHz);
            expectRejected (*this, "100 db", Kind::percent, ParamRanges::noiseAmount);

            expectRejected (*this, "1:3", Kind::ratio, ParamRanges::compRatio);
            expectRejected (*this, "3:2", Kind::ratio, ParamRanges::compRatio);
            expectRejected (*this, "3:1:1", Kind::ratio, ParamRanges::compRatio);
            expectRejected (*this, "ratio", Kind::ratio, ParamRanges::compRatio);
        }

        beginTest ("format then parse returns the same value for every kind");
        {
            // All three fit the pre-gain range the round trip parses with.
            const float dbs[] { -12.0f, 0.0f, 6.0f };

            for (const auto db : dbs)
                expectParsed (*this, ValueText::format (db, Kind::decibels),
                              Kind::decibels, ParamRanges::preGainDb, db);

            expectParsed (*this, ValueText::format (80.0f, Kind::hertz),
                          Kind::hertz, ParamRanges::hpfHz, 80.0f);
            expectParsed (*this, ValueText::format (100.0f, Kind::percent),
                          Kind::percent, ParamRanges::noiseAmount, 100.0f);
            expectParsed (*this, ValueText::format (3.0f, Kind::ratio),
                          Kind::ratio, ParamRanges::compRatio, 3.0f);
        }

        beginTest ("unit strings map to the right kind");
        {
            expect (ValueText::kindForUnit ("dB") == Kind::decibels);
            expect (ValueText::kindForUnit ("Hz") == Kind::hertz);
            expect (ValueText::kindForUnit ("%") == Kind::percent);
            expect (ValueText::kindForUnit (":1") == Kind::ratio);
        }
    }
};

static HotkeyStatusTests hotkeyStatusTests;
static ValueTextTests valueTextTests;
