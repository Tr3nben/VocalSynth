#include <catch2/catch_session.hpp>
#include <juce_gui_basics/juce_gui_basics.h>

int main (int argc, char* argv[])
{
    // The APVTS and timers need a MessageManager; this sets it up and tears it down cleanly
    juce::ScopedJuceInitialiser_GUI juceInitialiser;
    return Catch::Session().run (argc, argv);
}
