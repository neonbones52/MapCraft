#pragma once

#include <cstdint>
#include <vector>

#include "ui/screens/screen.h"

namespace mc
{
    // The Dev window's own state (UI state, not the world's): the enemy's provinces ticked for a demand.
    struct DevWindowState
    {
        std::vector<std::uint16_t> demand;
    };

    // The "Dev" window (F9, docs/CODE.md, "Отладочное"): debug actions as the player's ordinary commands, a stand-in
    // until the real UI (M13). Now: the wars going on with their war scores (game/war_score.h); for each war of the
    // one played, a peace: its enemy's provinces to tick, their price and whether the enemy would accept (the rule
    // of game/peace.h), "Demand" and "White peace"; "Declare war" on each other country.
    void dev_window(const ScreenContext& ui, DevWindowState* state, UiCommands* commands, bool* open);
}
