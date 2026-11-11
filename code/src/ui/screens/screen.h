#pragma once

#include "game/calendar.h"

namespace mc
{
    class MapData;
    class Session;
    class FlagCache;
    struct Content;
    struct WorldState;

    // What every screen reads (the game's state is read-only to the UI) and the few UI services it uses.
    struct ScreenContext
    {
        const MapData& map;
        const Content& content;    // the World's, read-only like everything here
        const WorldState& state;
        bool loaded = false;       // the world has its data (else nothing to show but a note)
        const Session& session;
        FlagCache& flags;
        float width = 0.0f;    // viewport px
        float height = 0.0f;
        float scale = 1.0f;    // UI scale (ImGuiLayer::scale)
        Date date;                    // the world's today
    };

    // What the player asked for this frame; app/shell.h carries it out on the Session (screens change nothing).
    struct UiCommands
    {
        bool play = false;       // start as the selected country
        bool new_game = false;   // back to the country selection
        int set_speed = 0;       // 1..5: that speed (game/pace.h); 0 = leave it
        bool toggle_pause = false;
    };
}
