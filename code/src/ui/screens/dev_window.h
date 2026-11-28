#pragma once

#include "ui/screens/screen.h"

namespace mc
{
    // The "Dev" window (F9, docs/CODE.md, "Отладочное"): debug actions as the player's ordinary commands, a stand-in
    // until the real UI (M13). Now: the wars going on, and "Declare war" on each other country for the one played.
    void dev_window(const ScreenContext& ui, UiCommands* commands, bool* open);
}
