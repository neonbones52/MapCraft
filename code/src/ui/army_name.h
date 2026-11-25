#pragma once

#include <cstdint>
#include <format>
#include <string>

// What the player calls an army: not data (the user's decision, M5 step 4), its slot's number as an ordinal,
// "1st Army". The same army keeps its name while it lives; a new one in a freed slot takes that slot's name.
namespace mc::ui
{
    [[nodiscard]] inline std::string army_name(std::uint32_t slot)
    {
        const std::uint32_t n = slot + 1;
        const std::uint32_t tens = n % 100;
        const char* suffix = (tens >= 11 && tens <= 13) ? "th"
                             : n % 10 == 1               ? "st"
                             : n % 10 == 2               ? "nd"
                             : n % 10 == 3               ? "rd"
                                                         : "th";
        return std::format("{}{} Army", n, suffix);
    }
}
