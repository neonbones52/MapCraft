#pragma once

#include "ui/screens/screen.h"

namespace mc
{
    class BannerCache;
    class Windows;

    // The top left corner (docs/UI.md, "Раскладка экрана"): the player's banner on a panel, and to its right the top
    // bar: the country's name, its treasury with the monthly income, its manpower, its inflation, each with a tooltip
    // of its parts. The banner opens and closes the country window (ui/windows.h). Without a country (the country
    // selection, an observer) the observer's banner alone.
    void top_bar(const ScreenContext& ui, BannerCache* banners, Windows* windows);

    // Where the left zone's windows begin, screen px from the top: under the banner's panel.
    [[nodiscard]] float left_zone_top();
    // The screen px margin the fixed panels keep from the screen's edges (4 GUI px).
    [[nodiscard]] float screen_margin();
}
