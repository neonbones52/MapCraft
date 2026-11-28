#pragma once

#include <cstdint>

#include "game/economy/income.h"
#include "game/economy/upkeep.h"
#include "game/game_data.h"

// The treasury and the budget sliders (docs/GAMEDATA.md, "Казна"; EU3's two systems of money, docs/ECONOMY.md):
//   the year    on 1 January the census (game/economy/income.h) goes into the treasury
//   the month   the monthly income (tax, production and gold, game/economy/income.h; trade later) is not the
//               treasury's: the budget sliders share it among the five accounts (Account: Army, Production,
//               Government, stability, Treasury), exactly (ledger::mint_shares); the Treasury share raises the
//               inflation by
//               budget_treasury / 100 × treasury_inflation / 12 a month, if it brought anything. The investments
//               only pile up: what they buy is M9's. Then the army's upkeep (game/economy/upkeep.h) goes out of
//               the treasury.
// Owned by World, which calls census and then month on 1 January (World::tick_month); its data is the working columns.

namespace mc
{
    class Budget
    {
    public:
        // 1 January: every country's census into its treasury.
        void census(WorldState& state, const Content& content);
        // The 1st of each month: every country's monthly income through its sliders, and the inflation.
        void month(WorldState& state, const Content& content);

    private:
        WorldIncome income_;
        WorldUpkeep upkeep_;
    };

    // One country's sliders.
    [[nodiscard]] BudgetShares budget_shares(const CountryStates& countries, std::uint16_t country);
    // Why `country` cannot set its sliders to `shares` (the command `budget`): no such country, or the shares do not
    // add up to 100; nullptr if it can.
    [[nodiscard]] const char* why_no_budget(const WorldState& state, std::uint16_t country,
                                            const BudgetShares& shares);
    // Sets them (checked by why_no_budget first).
    void set_budget(WorldState& state, std::uint16_t country, const BudgetShares& shares);
}
