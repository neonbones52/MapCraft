#pragma once

#include <cstdint>
#include <string>

namespace mc
{
    class World;
}

// The saved state (game/saved_state.h) as mapcraft_sim sees it: its checksum and its dump.

namespace mc::headless
{
    // The state's checksum (CLAUDE.md, rule 11: here and only here): every saved column's name, length and raw bytes
    // in for_each_saved's order, then the day, through a 64-bit FNV-1a-style hash taken a 64-bit word at a time.
    [[nodiscard]] std::uint64_t checksum(const World& world);

    // Every saved column, element by element (padding included, as the checksum sees it), as CSV: column,index,value;
    // the first row is the day. Floats print in their shortest exact form, so the dump reads back to the same bits.
    [[nodiscard]] std::string dump(const World& world);
}
