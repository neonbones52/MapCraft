#pragma once

#include <cstdint>

#include "ui/screens/screen.h"

namespace mc
{
    // The province under the cursor, beside the cursor (both phases): its owner and name, and the armies standing in
    // it (regiments, men, morale). Nothing for a province with neither an owner nor an army.
    void province_tooltip(const ScreenContext& ui, std::uint32_t province, float cursor_x, float cursor_y);
}
