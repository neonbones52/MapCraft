#pragma once

#include "ui/screens/screen.h"

namespace mc
{
    // While playing (Session::Phase::playing): the player's country top left. A debug stand-in for the top bar.
    void player_hud(const ScreenContext& ui, UiCommands* commands);
}
