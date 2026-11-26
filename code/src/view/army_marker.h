#pragma once

#include <cstdint>
#include <span>

namespace mc
{
    // Where an army's plate goes this frame (app/army_figures.h makes them, ui/screens/army_plates.h draws them):
    // the screen point under its figure's feet.
    struct ArmyMarker
    {
        std::uint16_t army = 0;   // its slot (game/armies.h)
        float x = 0.0f;           // screen px
        float y = 0.0f;
    };

    // This frame's plates and how much they show (0 hidden, 1 whole): the figures' fade.
    struct ArmyMarkers
    {
        std::span<const ArmyMarker> markers;
        float fade = 0.0f;
    };
}
