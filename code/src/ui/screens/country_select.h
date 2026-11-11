#pragma once

#include <cstdint>

#include "ui/screens/screen.h"

namespace mc
{
    // The country selection (Session::Phase::choose_country): the selected country's summary top right, Play bottom
    // right. Countries are chosen by clicking the map (app/shell.h).
    void country_select_screen(const ScreenContext& ui, UiCommands* commands);

    // The owner and name of the province under the cursor, beside the cursor.
    void country_tooltip(const ScreenContext& ui, std::uint32_t province, float cursor_x, float cursor_y);
}
