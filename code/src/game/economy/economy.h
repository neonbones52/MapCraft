#pragma once

#include <cstddef>
#include <cstdint>

#include "game/game_data.h"

// The economy's day: its phases, in the one order written in economy.cpp (docs/ECONOMY.md, "Код").

namespace mc
{
    // One day: mining, production, then every market clears each tier of needs (docs/GAMEDATA.md, "Экономика дня").
    void economy_day(const Content& content, WorldState* state);

    // How much of a tier of needs the province's pops got on the last day, weighted by people: 0..1, or -1 when no pop
    // of the province needs that tier (UI).
    [[nodiscard]] float needs_filled(const Content& content, const WorldState& state, std::uint32_t province,
                                     std::size_t tier);
}
