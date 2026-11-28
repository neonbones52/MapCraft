#pragma once

#include <cstdint>

#include "game/game_data.h"

// The provinces' income (docs/GAMEDATA.md, "Казна", "Производство"; EU3's, docs/ECONOMY.md): the formulas the monthly
// and the yearly tick and the UI share. A province pays its owner only while the owner controls it: an occupied one
// pays no one, neither tax nor production.
//   the base          base_tax, + capital_tax if it is its owner's seat (game/countries.h)
//   the tax a month   base / 12
//   the census        base × census_core with its owner's core, × census_no_core without: on 1 January, into the
//                     treasury
//   the volume        what it produces of its good a year (EU3 In Nomine's units): production_base + population /
//                     population_per_unit + base_tax / base_tax_per_unit
//   production        volume / 12 × its good's price (goods.csv's base price: no world price yet) a month
//   gold              the money good's (emerald, EU3's gold) instead of production: volume / 12 emeralds a month
// The monthly income is tax + production + gold (the budget sliders share it, game/economy/budget.h). No modifiers
// and no production efficiency yet (stability, technology, the domestic sliders: M9). Worked out in double; rounded
// to grains once a country, by the rule that pays it.

namespace mc
{
    struct ProvinceIncome
    {
        double tax = 0.0;          // a month
        double production = 0.0;   // a month
        double gold = 0.0;         // a month
        double census = 0.0;       // a year

        [[nodiscard]] double monthly() const { return tax + production + gold; }
    };

    // What province `p` produces of its good a year, whoever controls it.
    [[nodiscard]] double province_volume(const WorldState& state, const EconomyDefines& defines, std::uint32_t p);
    // What province `p` pays its owner; `seat`: its owner's seat (game/countries.h, seat()).
    [[nodiscard]] ProvinceIncome province_income(const WorldState& state, const Content& content, std::uint32_t p,
                                                 std::uint32_t seat);

    // Every country's, in one pass over the provinces (in id order): the columns are indexed like CountryStates. The
    // ticks' working columns: they keep their room between months.
    struct WorldIncome
    {
        Column<std::uint32_t> seat;     // each country's seat
        Column<ProvinceIncome> sum;     // each country's income: its provinces' summed
    };
    void world_income(const WorldState& state, const Content& content, WorldIncome* out);

    // One country's (the UI's, mapcraft_sim's series): the same sums, in the same order.
    [[nodiscard]] ProvinceIncome country_income(const WorldState& state, const Content& content,
                                                std::uint16_t country);
}
