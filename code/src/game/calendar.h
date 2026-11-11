#pragma once

#include <cstdint>
#include <string>
#include <string_view>

namespace mc
{
    // The game's calendar, a placeholder until the setting's own is specified: twelve months of 30 days, so a year is
    // 360 days. Time is counted in days from 1 January, year 1 (day 0); a Date is that number read as a year, a month
    // and a day of the month. No state: World holds the current day.
    inline constexpr std::uint32_t kDaysPerMonth = 30;
    inline constexpr std::uint32_t kMonthsPerYear = 12;
    inline constexpr std::uint32_t kDaysPerYear = kDaysPerMonth * kMonthsPerYear;

    struct Date
    {
        std::uint32_t year = 1;
        std::uint8_t month = 1;   // 1..12
        std::uint8_t day = 1;     // 1..30
    };

    [[nodiscard]] constexpr std::uint32_t day_number(Date date)
    {
        return ((date.year - 1) * kMonthsPerYear + (date.month - 1)) * kDaysPerMonth + (date.day - 1);
    }

    [[nodiscard]] constexpr Date date_of(std::uint32_t day)
    {
        const std::uint32_t months = day / kDaysPerMonth;   // whole months since day 0
        return Date{ months / kMonthsPerYear + 1, static_cast<std::uint8_t>(months % kMonthsPerYear + 1),
                     static_cast<std::uint8_t>(day % kDaysPerMonth + 1) };
    }

    // The start of the game (a placeholder): 1 January, year 1.
    inline constexpr Date kStartDate{ 1, 1, 1 };
    inline constexpr std::uint32_t kStartDay = day_number(kStartDate);
    static_assert(date_of(kStartDay).year == kStartDate.year && date_of(kStartDay).month == kStartDate.month &&
                  date_of(kStartDay).day == kStartDate.day);

    // "August" for month 8 (January first); "" for anything else.
    [[nodiscard]] std::string_view month_name(unsigned month);
    // "17 August 201".
    [[nodiscard]] std::string format(Date date);
}
