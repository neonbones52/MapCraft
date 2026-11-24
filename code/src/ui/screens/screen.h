#pragma once

#include <cstdint>

#include "game/calendar.h"

namespace mc
{
    class MapView;
    class Session;
    class Pace;
    class FlagCache;
    struct Content;
    struct WorldState;

    // What every screen reads (the game's state is read-only to the UI) and the few UI services it uses.
    struct ScreenContext
    {
        const MapView& map;
        const Content& content;    // the World's, read-only like everything here
        const WorldState& state;
        bool loaded = false;       // the world has its data (else nothing to show but a note)
        const Session& session;
        const Pace& pace;          // the speed and the pause (view/pace.h)
        FlagCache& flags;
        float width = 0.0f;    // viewport px
        float height = 0.0f;
        float scale = 1.0f;    // UI scale (ImGuiLayer::scale)
        Date date;                    // the world's today
        std::uint8_t price_good = 0;  // the good the Prices map mode shows, 0 = that mode is off
    };

    // What the player asked for this frame; app/shell.h carries it out on the Session and the Pace (screens change
    // nothing).
    struct UiCommands
    {
        bool play = false;       // start as the selected country
        bool new_game = false;   // back to the country selection
        int set_speed = 0;       // 1..5: that speed (view/pace.h); 0 = leave it
        bool toggle_pause = false;
        int show_prices = -1;    // a good: the Prices map mode with it; 0: back to the political map; -1: leave it
    };
}
