#pragma once

#include "ui/screens/screen.h"

namespace mc
{
    // The market prices (Session::Phase::playing), bottom left: pick a good to see its price on every market in the
    // Prices map mode, or Off to go back to the political map.
    void prices_panel(const ScreenContext& ui, UiCommands* commands);
}
