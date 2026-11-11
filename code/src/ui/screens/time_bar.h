#pragma once

#include "ui/screens/screen.h"

namespace mc
{
    // While playing (Session::Phase::playing): today's date and the time controls (pause, speed 1..5), top right.
    // A debug stand-in for the top bar.
    void time_bar(const ScreenContext& ui, UiCommands* commands);
}
