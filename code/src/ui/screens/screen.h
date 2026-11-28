#pragma once

#include <cstdint>
#include <vector>

#include "game/calendar.h"
#include "game/game_data.h"

struct ImFont;

namespace mc
{
    class MapView;
    class Session;
    class Pace;
    class FlagCache;
    struct Content;
    struct WorldState;
    struct Cache;

    // What every screen reads (the game's state is read-only to the UI) and the few UI services it uses.
    struct ScreenContext
    {
        const MapView& map;
        const Content& content;    // the World's, read-only like everything here
        const WorldState& state;
        const Cache& cache;        // the modifier sums (game/modifiers.h)
        bool loaded = false;       // the world has its data (else nothing to show but a note)
        const Session& session;
        const Pace& pace;          // the speed and the pause (view/pace.h)
        FlagCache& flags;
        float width = 0.0f;    // viewport px
        float height = 0.0f;
        float scale = 1.0f;    // UI scale (ImGuiLayer::scale)
        Date date;             // the world's today
        ImFont* bold = nullptr;   // the plates' font (ImGuiLayer::bold)
    };

    // What the player asked for this frame; app/shell.h carries it out on the Session and the Pace (screens change
    // nothing).
    struct UiCommands
    {
        bool play = false;       // start as the selected country
        bool new_game = false;   // back to the country selection
        int set_speed = 0;       // 1..5: that speed (view/pace.h); 0 = leave it
        bool toggle_pause = false;
        std::uint16_t declare_war = 0;   // the country the player declares war on (the Dev window); 0 = none
        std::uint16_t peace_with = 0;    // the country the player offers peace to (the Dev window); 0 = none
        std::vector<std::uint16_t> peace_provinces;   // of it, taken by that peace; none: a white peace
        bool set_budget = false;   // the player sets their budget sliders to `budget` (the Dev window)
        BudgetShares budget{};
    };
}
