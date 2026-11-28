#pragma once

#include <cstdint>

#include "game/game_data.h"

// The manpower (docs/GAMEDATA.md, "Людские ресурсы и найм"; the user's decisions 28.11.2026, EU4's ways):
//   the maximum   manpower_base for every country + Σ over the provinces it owns and controls (an occupied one gives
//                 none, as in the income) of their population × manpower_core with its core, × manpower_no_core
//                 without; whole men, rounded down. Modifiers (its share, the recovery's speed) come later.
//   the month     the pool grows by maximum / (manpower_recovery_years × 12), whole men rounded down, up to the
//                 maximum; a pool above it (provinces lost) falls to it
//   the start     manpower_start of the maximum, rounded down (GameData::load)
// The recruitment takes kFullRegiment men a regiment out of the pool when it is ordered (game/recruitment.h).

namespace mc
{
    // The country's maximum, men.
    [[nodiscard]] std::int32_t max_manpower(const WorldState& state, const Content& content, std::uint16_t country);
    // What a pool of that maximum grows by a month, men.
    [[nodiscard]] std::int32_t monthly_manpower(const EconomyDefines& defines, std::int32_t maximum);

    // Every country's maximum, in one pass over the provinces (in id order): the columns are indexed like
    // CountryStates; the tick's working columns keep their room between months.
    struct WorldManpower
    {
        Column<double> sum;            // working: each country's maximum before rounding
        Column<std::int32_t> maximum;
    };
    void world_max_manpower(const WorldState& state, const Content& content, WorldManpower* out);

    // The start's pools (GameData::load).
    void fill_manpower(WorldState& state, const Content& content);

    // The monthly tick of the pools (World::tick_month, after the budget). Owned by World; its data is the working
    // columns.
    class Manpower
    {
    public:
        void month(WorldState& state, const Content& content);

    private:
        WorldManpower work_;
    };
}
