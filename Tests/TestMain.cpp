// Runner for every juce::UnitTest linked into this executable. No device or
// window is needed: clocks, drift and plug-in latency are simulated.
//
//   cmake --build build --config Release --target OneCastTests
//   build\OneCastTests_artefacts\Release\OneCastTests.exe            all categories
//   build\OneCastTests_artefacts\Release\OneCastTests.exe <category> one category
//
// The exit code is 0 only when every test that ran passed.

#include <JuceHeader.h>

#include <cstdio>

int main (int argc, char** argv)
{
    // Timers, async updaters and the like need a message manager to exist.
    juce::ScopedJuceInitialiser_GUI juceInitialiser;

    juce::UnitTestRunner runner;
    runner.setAssertOnFailure (false);

    if (argc > 1)
        runner.runTestsInCategory (argv[1]);
    else
        runner.runAllTests();

    // JUCE logs through juce::Logger, which a console app does not show, so
    // print the results explicitly.
    int failures = 0;
    for (int i = 0; i < runner.getNumResults(); ++i)
    {
        const auto* r = runner.getResult (i);
        failures += r->failures;

        std::printf ("%-60s %3d passed, %d failed\n",
                     (r->unitTestName + " / " + r->subcategoryName).toRawUTF8(),
                     r->passes, r->failures);

        for (const auto& m : r->messages)
            std::printf ("    %s\n", m.toRawUTF8());
    }

    // A mistyped category would otherwise run nothing and report success.
    if (runner.getNumResults() == 0)
    {
        std::printf ("\nFAIL: no tests ran%s%s\n",
                     argc > 1 ? " in category " : "", argc > 1 ? argv[1] : "");
        return 1;
    }

    std::printf ("\n%s: %d failure(s)\n", failures == 0 ? "PASS" : "FAIL", failures);
    return failures == 0 ? 0 : 1;
}
