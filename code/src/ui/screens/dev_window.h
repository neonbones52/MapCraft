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
        int maintenance = 100;            // the maintenance slider while dragged (`maintenance_edited`)
        bool maintenance_edited = false;
        std::uint16_t recruit_province = 0;   // where to recruit (0: the player's seat)
        std::uint8_t recruit_type = 1;        // the regiment type to recruit
    };

    // The "Dev" window (F9, docs/CODE.md, "Отладочное"): debug actions as the player's ordinary commands, a stand-in
    // until the real UI's frame (M8). Now: the player's treasury, monthly income, census, inflation and investments, and
    // the five budget sliders, which move together to 100 and are set when let go (the command budget,
    // game/economy/budget.h); the army's upkeep and force limit, the maintenance slider, set when let go (the
    // command maintenance, game/economy/upkeep.h); the manpower pool, its maximum and growth (game/economy/manpower.h),
    // a province of the player's and a regiment type with its cost and time, "Recruit" (the command recruit,
    // game/recruitment.h) and the regiments ordered; the wars going on with their war scores (game/war_score.h); for each war of the
    // one played, a peace: its enemy's provinces to tick, their price and whether the enemy would accept (the rule
    // of game/peace.h), "Demand" and "White peace"; "Declare war" on each other country.
    void dev_window(const ScreenContext& ui, DevWindowState* state, UiCommands* commands, bool* open);
}
