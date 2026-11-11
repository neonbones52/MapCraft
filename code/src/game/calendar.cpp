#include "game/calendar.h"

#include <array>
#include <format>

namespace mc
{
    namespace
    {
        constexpr std::array<std::string_view, kMonthsPerYear> kMonthNames = {
            "January", "February", "March", "April", "May", "June",
            "July", "August", "September", "October", "November", "December",
        };
    }

    std::string_view month_name(unsigned month)
    {
        return month >= 1 && month <= kMonthsPerYear ? kMonthNames[month - 1] : std::string_view{};
    }

    std::string format(Date date)
    {
        return std::format("{} {} {}", date.day, month_name(date.month), date.year);
    }
}
