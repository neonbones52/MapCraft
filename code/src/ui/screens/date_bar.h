#pragma once

#include "ui/screens/screen.h"

namespace mc
{
    // The top right corner (docs/UI.md, "Раскладка экрана"): the date, the pause and the speed (view/pace.h).
    void date_bar(const ScreenContext& ui, UiCommands* commands);
}
