#include <JuceHeader.h>
#include "MainComponent.h"
#include "UI/Theme.h"
#include "Diagnostics/StartupLog.h"

class OneCastApplication : public juce::JUCEApplication
{
public:
    const juce::String getApplicationName() override    { return JUCE_APPLICATION_NAME_STRING; }
    const juce::String getApplicationVersion() override { return JUCE_APPLICATION_VERSION_STRING; }
    bool moreThanOneInstanceAllowed() override           { return false; }

    void initialise (const juce::String&) override
    {
        // Install the themed LookAndFeel before any window exists, so the pieces
        // that are only reachable through the default LookAndFeel -- popup menus,
        // scrollbars, tooltips, text editors -- match the rest of the UI.
        juce::Desktop::getInstance().setDefaultLookAndFeel (&themeLookAndFeel);

        mainWindow = std::make_unique<MainWindow> (getApplicationName());
    }

    void shutdown() override
    {
        mainWindow.reset();

        // Every component that referenced it is gone by now.
        juce::Desktop::getInstance().setDefaultLookAndFeel (nullptr);
    }

    void systemRequestedQuit() override
    {
        quit();
    }

    class MainWindow : public juce::DocumentWindow
    {
    public:
        explicit MainWindow (const juce::String& name)
            : DocumentWindow (name,
                              juce::Desktop::getInstance().getDefaultLookAndFeel()
                                  .findColour (juce::ResizableWindow::backgroundColourId),
                              DocumentWindow::allButtons)
        {
            setUsingNativeTitleBar (true);
            setContentOwned (new MainComponent(), true);
            setResizable (true, true);

            // Respect the content's own preferred size.
            centreWithSize (getWidth(), getHeight());
            setVisible (true);

            // Everything before this point delayed the first paint.
            StartupLog::mark ("window visible (first paint)");
            StartupLog::flush();
        }

        void closeButtonPressed() override
        {
            JUCEApplication::getInstance()->systemRequestedQuit();
        }

    private:
        JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (MainWindow)
    };

private:
    // Declared before mainWindow so it outlives every component that uses it.
    Theme::LookAndFeel themeLookAndFeel;
    std::unique_ptr<MainWindow> mainWindow;
};

START_JUCE_APPLICATION (OneCastApplication)
