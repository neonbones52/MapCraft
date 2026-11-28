#pragma once

#include <cstdint>

#include "game/game_data.h"

// The provinces' taxes (docs/GAMEDATA.md, "Казна"; EU3's, docs/ECONOMY.md): the formulas the monthly and the yearly
// tick and the UI share. A province pays its owner only while the owner controls it: an occupied one pays no one.
// Its base is its base_tax, + capital_tax if it is its owner's seat (game/countries.h):
//   the tax a month   base / 12, into the monthly income (the budget sliders share it, game/economy/budget.h)
//   the census        base × census_core with its owner's core, × census_no_core without: on 1 January, into the
//                     treasury
// No modifiers yet (stability, the domestic sliders: M9). Worked out in double; rounded to grains once a country, by
// the rule that pays it.

namespace mc
{
    struct ProvinceIncome
    {
        double tax = 0.0;      // a month
        double census = 0.0;   // a year
    };

    // What province `p` pays its owner; `seat`: its owner's seat (game/countries.h, seat()).
    [[nodiscard]] ProvinceIncome province_income(const WorldState& state, const EconomyDefines& defines,
                                                 std::uint32_t p, std::uint32_t seat);

    // Every country's, in one pass over the provinces (in id order): the columns are indexed like CountryStates. The
    // ticks' working columns: they keep their room between months.
    struct WorldIncome
    {
        Column<std::uint32_t> seat;   // each country's seat
        Column<double> tax;
        Column<double> census;
    };
    void world_income(const WorldState& state, const Content& content, WorldIncome* out);

    // One country's (the UI's): the same sums.
    [[nodiscard]] ProvinceIncome country_income(const WorldState& state, const Content& content,
                                                std::uint16_t country);
}
