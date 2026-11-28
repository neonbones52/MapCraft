#pragma once

#include <cstdint>
#include <string_view>

// Fixed point as EU4's CFixedPoint (docs/SIMULATION.md, "Числа"): an i32, kFixedOne to the unit, truncated toward
// zero after every multiplication and division, so the battle's numbers and roundings follow eu4.exe operation for
// operation. Used by the modifiers and the battle (morale, discipline, the shots); the rest of the simulation stays
// float. Integer arithmetic only: the same bits on every machine by construction.

namespace mc
{
    using Fixed = std::int32_t;
    inline constexpr Fixed kFixedOne = 1000;

    // a × b, truncated toward zero.
    [[nodiscard]] constexpr Fixed fx_mul(std::int64_t a, std::int64_t b)
    {
        return static_cast<Fixed>(a * b / kFixedOne);
    }
    // a / b, truncated toward zero; b != 0.
    [[nodiscard]] constexpr Fixed fx_div(std::int64_t a, std::int64_t b)
    {
        return static_cast<Fixed>(a * kFixedOne / b);
    }
    [[nodiscard]] constexpr Fixed fx_from_int(std::int32_t n) { return n * kFixedOne; }    // The whole part, truncated toward zero.
    [[nodiscard]] constexpr std::int32_t fx_to_int(Fixed f) { return f / kFixedOne; }
    // For display only (UI, logs): never back into the simulation.
    [[nodiscard]] constexpr double fx_to_double(Fixed f) { return static_cast<double>(f) / kFixedOne; }

    // A decimal number as written in the data ("-0.05", "2.5", "15") to fixed point, exactly: at most three digits
    // after the point, no exponent, no float on the way. False on anything else or out of range.
    [[nodiscard]] constexpr bool fx_parse(std::string_view text, Fixed* out)
    {
        std::size_t i = 0;
        const bool negative = !text.empty() && text[0] == '-';
        if (negative || (!text.empty() && text[0] == '+')) {
            ++i;
        }
        std::int64_t whole = 0;
        std::size_t digits = 0;
        for (; i < text.size() && text[i] >= '0' && text[i] <= '9'; ++i, ++digits) {
            whole = whole * 10 + (text[i] - '0');
            if (whole > 2'000'000) {
                return false;
            }
        }
        std::int64_t part = 0;
        std::int64_t scale = kFixedOne;
        if (i < text.size() && text[i] == '.') {
            ++i;
            for (; i < text.size() && text[i] >= '0' && text[i] <= '9'; ++i, ++digits) {
                if (scale == 1) {
                    return false;   // a fourth digit after the point
                }
                scale /= 10;
                part += (text[i] - '0') * scale;
            }
        }
        if (digits == 0 || i != text.size()) {
            return false;
        }
        const std::int64_t value = whole * kFixedOne + part;
        *out = static_cast<Fixed>(negative ? -value : value);
        return true;
    }
}
