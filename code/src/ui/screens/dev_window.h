#pragma once

#include <cstdint>
#include <vector>

#include "ui/screens/screen.h"

namespace mc
{
    // The Dev window's own state (UI state, not the world's): the enemy's provinces ticked for a demand; the budget
    // sliders while the player drags one (`budget_edited`), and as they were when the drag began (the others move in
    // proportion to those).
    struct DevWindowState
    {
        std::vector<std::uint16_t> demand;
        BudgetShares budget{};
        BudgetShares budget_start{};
        bool budget_edited = false;
    };

    // The "Dev" window (F9, docs/CODE.md, "Отладочное"): debug actions as the player's ordinary commands, a stand-in
    // until the real UI (M13). Now: the player's treasury, monthly income, census, inflation and investments, and
    // the five budget sliders, which move together to 100 and are set when let go (the command budget,
    // game/economy/budget.h); the wars going on with their war scores (game/war_score.h); for each war of the
    // one played, a peace: its enemy's provinces to tick, their price and whether the enemy would accept (the rule
    // of game/peace.h), "Demand" and "White peace"; "Declare war" on each other country.
    void dev_window(const ScreenContext& ui, DevWindowState* state, UiCommands* commands, bool* open);
}
