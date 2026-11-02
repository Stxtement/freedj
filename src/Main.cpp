#include "MainComponent.h"
#include <juce_gui_basics/juce_gui_basics.h>

namespace pathboard
{

class FreeDJApplication : public juce::JUCEApplication
{
public:
    const juce::String getApplicationName() override    { return JUCE_APPLICATION_NAME_STRING; }
    const juce::String getApplicationVersion() override { return JUCE_APPLICATION_VERSION_STRING; }
    bool moreThanOneInstanceAllowed() override           { return false; }

    void initialise (const juce::String&) override
    {
        juce::PropertiesFile::Options options;
        options.applicationName = "FreeDJ";
        options.folderName = "PathboardDJ";
        options.filenameSuffix = ".settings";
        options.osxLibrarySubFolder = "Application Support";
        properties.setStorageParameters (options);

        mainWindow = std::make_unique<MainWindow> (getApplicationName(), *properties.getUserSettings());
    }

    void shutdown() override
    {
        mainWindow = nullptr;
        properties.closeFiles();
    }

    void systemRequestedQuit() override { quit(); }

private:
    class MainWindow : public juce::DocumentWindow
    {
    public:
        MainWindow (const juce::String& name, juce::PropertiesFile& settings)
            : DocumentWindow (name, juce::Colour (0xff0e1014), DocumentWindow::allButtons)
        {
            setUsingNativeTitleBar (true);
            setContentOwned (new MainComponent (settings), true);
            setResizable (true, true);
            setResizeLimits (1100, 700, 10000, 10000);
            centreWithSize (getWidth(), getHeight());
            setVisible (true);
        }

        void closeButtonPressed() override
        {
            juce::JUCEApplication::getInstance()->systemRequestedQuit();
        }
    };

    juce::ApplicationProperties properties;
    std::unique_ptr<MainWindow> mainWindow;
};

} // namespace pathboard

START_JUCE_APPLICATION (pathboard::FreeDJApplication)
