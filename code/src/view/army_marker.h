#pragma once

#include <cstdint>
#include <span>

namespace mc
{
    // Where an army is on the screen this frame (app/army_figures.h makes them, ui/screens/army_plates.h draws the
    // plates, app/army_orders.h picks armies by them): the point under its figure's feet where its plate goes, and
    // the figure's box.
    struct ArmyMarker
    {
        std::uint16_t army = 0;   // its slot (game/armies.h)
        float x = 0.0f;           // screen px: the plate's top middle
        float y = 0.0f;
        float top = 0.0f;         // screen px: the figure's box, x +- half_width, top..bottom
        float bottom = 0.0f;
        float half_width = 0.0f;
    };

    // A screen rectangle being dragged out to select armies (app/army_orders.h), corner to corner.
    struct SelectionBox
    {
        bool shown = false;
        float x0 = 0.0f;
        float y0 = 0.0f;
        float x1 = 0.0f;
        float y1 = 0.0f;
    };

    // This frame's armies on the screen: their markers and how much they show (0 hidden, 1 whole: the figures'
    // fade), the player's selected ones (slots, ascending) and the box being dragged.
    struct ArmyMarkers
    {
        std::span<const ArmyMarker> markers;
        float fade = 0.0f;
        std::span<const std::uint16_t> selected;
        SelectionBox box;
    };
}
