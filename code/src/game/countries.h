#pragma once

#include <cstddef>
#include <cstdint>

#include "game/game_data.h"

namespace mc
{
    // Queries over the world state for one country (loops over SoA columns; no strings, no UI).

    // Whether the country owns at least one province.
    [[nodiscard]] bool owns_land(const WorldState& state, std::uint16_t country);

    // The province the country is seated in: its capital if it owns it, else its most developed province; 0 = none.
    [[nodiscard]] std::uint32_t seat(const WorldState& state, std::uint16_t country);

    struct CountryStats
    {
        std::size_t provinces = 0;   // owned
        std::size_t cores = 0;       // provinces with its core, owned or not
        float tax = 0.0f;
        float production = 0.0f;
        float manpower = 0.0f;

        [[nodiscard]] float development() const { return tax + production + manpower; }
    };
    [[nodiscard]] CountryStats country_stats(const WorldState& state, std::uint16_t country);
}
