#pragma once

#include <cstdint>
#include <limits>

#include "core/fixed.h"
#include "game/game_data.h"

// The army's upkeep, the land force limit and the maintenance slider (docs/GAMEDATA.md, "Содержание армии"; the
// user's decisions 28.11.2026, EU3's numbers and EU4's ways):
//   the force limit   force_limit_base + force_limit_per_base_tax × (Σ base_tax of the provinces the country owns,
//                     occupied or not, + capital_tax at its seat), rounded down to whole regiments (1.8 is 1),
//                     never below 0: what the rules, the war score and the UI all read
//   the base upkeep   Σ over its regiments: strength / 1000 × its type's cost × land_maintenance (a month; EU4: a
//                     regiment pays for the men it has, but counts whole towards the force limit)
//   the upkeep        base × maintenance / 100 × (1 + max(0, force_limit_extra_cost × (regiments - limit) / limit))
//                     × (1 + inflation / 100): out of the treasury each month (game/economy/budget.h), which may go
//                     below 0 until loans and bankruptcy come
//   morale            the maintenance slider sets a ceiling on its regiments' morale: min_maintenance_morale of the
//                     maximum (game/modifiers.h) at maintenance_min, linear up to the maximum at 100. Below 100 the
//                     morale shown is the lesser of a regiment's and the ceiling; the monthly tick
//                     (game/recovery.h) and joining a battle (game/combat.h) cut it down to the ceiling, and the
//                     recovery fills it up to it. At 100 there is no ceiling (morale above the maximum stays).

namespace mc
{
    struct CountryModifiers;

    // No ceiling: the maintenance slider at 100.
    inline constexpr Fixed kNoMoraleCeiling = std::numeric_limits<Fixed>::max();

    // The country's land force limit, whole regiments.
    [[nodiscard]] std::uint32_t force_limit(const WorldState& state, const Content& content, std::uint16_t country);

    // The upkeep a month from its parts (the formula above).
    [[nodiscard]] double land_upkeep(const EconomyDefines& defines, double base, std::uint32_t regiments,
                                     std::uint32_t limit, std::uint8_t maintenance, float inflation);

    // One country's army and what it costs (the UI's, mapcraft_sim's series).
    struct ArmyUpkeep
    {
        double base = 0.0;
        std::uint32_t regiments = 0;
        std::uint32_t limit = 0;
        double upkeep = 0.0;   // a month
    };
    [[nodiscard]] ArmyUpkeep army_upkeep(const WorldState& state, const Content& content, std::uint16_t country);

    // Every country's, in one pass over the provinces and one over the regiments: the columns are indexed like
    // CountryStates. The tick's working columns: they keep their room between months.
    struct WorldUpkeep
    {
        Column<double> base_tax;   // working: the base_tax each country owns
        Column<std::uint32_t> limit;
        Column<double> base;
        Column<std::uint32_t> regiments;
    };
    void world_upkeep(const WorldState& state, const Content& content, WorldUpkeep* out);

    // The ceiling the country's maintenance sets on its regiments' morale; kNoMoraleCeiling at 100.
    [[nodiscard]] Fixed morale_ceiling(const CountryModifiers& sums, const CountryStates& countries,
                                       const EconomyDefines& defines, std::uint16_t country);
    // Why `country` cannot set its maintenance to `level` (the command maintenance): no such country, or not
    // maintenance_min..100; nullptr if it can.
    [[nodiscard]] const char* why_no_maintenance(const WorldState& state, const EconomyDefines& defines,
                                                 std::uint16_t country, std::uint8_t level);
}
