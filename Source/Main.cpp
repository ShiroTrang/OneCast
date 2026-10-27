#include <JuceHeader.h>
#include "MainComponent.h"
#include "UI/Theme.h"
#include "Diagnostics/StartupLog.h"

class OneCastApplication : public juce::JUCEApplication
{
public:
    /// Set by --minimized. Task 14 creates the window hidden when this is true
    /// (or when startMinimized is on in the settings); Task 12 only parses it.
    bool startMinimized = false;

    const juce::String getApplicationName() override    { return JUCE_APPLICATION_NAME_STRING; }
    const juce::String getApplicationVersion() override { return JUCE_APPLICATION_VERSION_STRING; }
    bool moreThanOneInstanceAllowed() override           { return false; }

    void initialise (const juce::String&) override
    {
        bool testSignal = false;
        juce::String snapshotPath;
        int snapshotDelayMs = 2000;

        const auto args = getCommandLineParameterArray();

        for (int i = 0; i < args.size(); ++i)
        {
            if (args[i] == "--test-signal")
                testSignal = true;
            else if (args[i] == "--minimized")
                startMinimized = true;
            else if (args[i] == "--snapshot" && i + 1 < args.size())
                snapshotPath = args[++i];
            else if (args[i] == "--snapshot-delay-ms" && i + 1 < args.size())
                snapshotDelayMs = juce::jlimit (0, 60000, args[++i].getIntValue());
        }

        // Install the themed LookAndFeel before any window exists, so the pieces
        // that are only reachable through the default LookAndFeel -- popup menus,
        // scrollbars, tooltips, text editors -- match the rest of the UI.
        juce::Desktop::getInstance().setDefaultLookAndFeel (&themeLookAndFeel);

        mainWindow = std::make_unique<MainWindow> (getApplicationName());

        auto* content = static_cast<MainComponent*> (mainWindow->getContentComponent());

        if (testSignal)
            content->getEngine().setTestSignal (true);

        if (snapshotPath.isNotEmpty())
            scheduleSnapshot (snapshotPath, snapshotDelayMs);
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
    /// The screenshot is taken on the message thread after the window has had
    /// time to paint, then the app quits. The safe pointer keeps a closed
    /// window from being touched if the user got there first.
    void scheduleSnapshot (juce::String path, int delayMs)
    {
        juce::Component::SafePointer<MainWindow> safeWindow (mainWindow.get());

        juce::Timer::callAfterDelay (delayMs, [safeWindow, path]()
        {
            if (auto* window = safeWindow.getComponent())
            {
                if (auto* content = window->getContentComponent())
                {
                    auto file = juce::File (path);

                    if (! juce::File::isAbsolutePath (path))
                        file = juce::File::getCurrentWorkingDirectory().getChildFile (path);

                    file.getParentDirectory().createDirectory();
                    file.deleteFile();

                    const auto image = content->createComponentSnapshot (content->getLocalBounds());

                    if (auto stream = std::unique_ptr<juce::FileOutputStream> (file.createOutputStream()))
                    {
                        if (stream->openedOk())
                        {
                            juce::PNGImageFormat png;
                            png.writeImageToStream (image, *stream);
                            stream->flush();
                        }
                    }
                }
            }

            juce::JUCEApplication::quit();
        });
    }

    // Declared before mainWindow so it outlives every component that uses it.
    Theme::LookAndFeel themeLookAndFeel;
    std::unique_ptr<MainWindow> mainWindow;
};

START_JUCE_APPLICATION (OneCastApplication)
