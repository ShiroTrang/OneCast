#include <JuceHeader.h>
#include "MainComponent.h"
#include "UI/Theme.h"
#include "Diagnostics/StartupLog.h"

class OneCastApplication : public juce::JUCEApplication,
                           private juce::Timer
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
            else if (args[i] == "--latency-log")
                latencyLog = true;
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

        // The content has loaded the settings by now. A quiet launch comes from
        // the autostart entry (--minimized) or from the saved option; the tray
        // icon is the way in, and a second instance also brings the window up.
        const bool startHidden = startMinimized || content->getSettings().startMinimized;

        content->onShowWindow = [this]() { showMainWindow(); };

        // Quit from the tray menu: the menu callback is running inside the tray
        // component, so the app is torn down on the next message instead of
        // under its own feet.
        content->onQuit = []()
        {
            juce::MessageManager::callAsync ([]()
            {
                if (auto* app = juce::JUCEApplication::getInstance())
                    app->quit();
            });
        };

        if (testSignal)
            content->getEngine().setTestSignal (true);

        if (snapshotPath.isNotEmpty())
        {
            // A screenshot needs a visible window even when the launch asked
            // for the tray; the app quits as soon as the PNG is written.
            showMainWindow();
            scheduleSnapshot (snapshotPath, snapshotDelayMs);
        }
        else if (! startHidden)
        {
            showMainWindow();
        }
        else
        {
            StartupLog::mark ("started minimised: window hidden");
            StartupLog::flush();
        }

        // --latency-log: keep writing the render-deadline counters while the
        // app runs so a probe run can be matched against them afterwards. The
        // message thread does the I/O; the render thread only stores atomics.
        if (latencyLog)
            startTimer (10000);
    }

    void shutdown() override
    {
        stopTimer();
        mainWindow.reset();

        // Every component that referenced it is gone by now.
        juce::Desktop::getInstance().setDefaultLookAndFeel (nullptr);
    }

    void anotherInstanceStarted (const juce::String&) override
    {
        // The second process hands over its command line and exits; the running
        // instance is what the user was trying to bring back.
        showMainWindow();
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

            // The cards need the full 1280 x 720 content area. setContentOwned
            // has already sized the window from the content, so the limits can
            // be applied here, before the user can grab an edge. Applying them
            // from the content's resized() was not reliable: that may never run
            // again once the window exists.
            if (auto* content = getContentComponent())
            {
                const int frameWidth  = juce::jmax (0, getWidth() - content->getWidth());
                const int frameHeight = juce::jmax (0, getHeight() - content->getHeight());

                setResizeLimits (1280 + frameWidth, 720 + frameHeight, 10000, 10000);
            }

            // Respect the content's own preferred size. Visibility is decided
            // by the application, because --minimized and the saved startMinimized
            // option open the window hidden in the tray.
            centreWithSize (getWidth(), getHeight());
        }

        /// Shows the window again after the close button hid it, after a quiet
        /// start, or when a second instance was launched.
        void showFromTray()
        {
            setVisible (true);
            setMinimised (false);
            toFront (true);

            // The first paint is the interesting one for the start-up log; a
            // return from the tray does not need a second entry.
            if (! hasBeenShown)
            {
                hasBeenShown = true;
                StartupLog::mark ("window visible (first paint)");
                StartupLog::flush();
            }
        }

        void closeButtonPressed() override
        {
            // The close button hides the window: the processing chain and the
            // tray icon must keep running. Quit lives in the tray menu.
            setVisible (false);
        }

    private:
        bool hasBeenShown = false;

        JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (MainWindow)
    };

private:
    void showMainWindow()
    {
        if (mainWindow != nullptr)
            mainWindow->showFromTray();
    }

    void timerCallback() override
    {
        if (mainWindow == nullptr)
            return;

        if (auto* content = static_cast<MainComponent*> (mainWindow->getContentComponent()))
        {
            StartupLog::mark ("latency: " + content->getEngine().describeLatency());
            StartupLog::flush();
        }
    }

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

                    StartupLog::mark ("latency: " + static_cast<MainComponent*> (content)->getEngine().describeLatency());

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

    /// Set by --latency-log.
    bool latencyLog = false;
};

START_JUCE_APPLICATION (OneCastApplication)
