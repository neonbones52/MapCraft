#pragma once

#include <cmath>
#include <cstdint>

// Money (docs/ECONOMY.md, "Деньги в i64"): whole grains in an i64, kGrainsPerEmerald to the emerald. Balances and
// claims are Money; everything else (prices, amounts of goods, shares) stays float. A sum is worked out in floating
// point, rounded to grains once (to_grains), and exactly that integer moves between accounts (game/economy/ledger.h).

namespace mc
{
    using Money = std::int64_t;
    inline constexpr Money kGrainsPerEmerald = 1'000'000;

    // Emeralds worked out in floating point, to the nearest grain (halves away from zero): the one rounding.
    [[nodiscard]] inline Money to_grains(double emeralds)
    {
        return static_cast<Money>(std::round(emeralds * static_cast<double>(kGrainsPerEmerald)));
    }

    // For display only (UI, logs): never back into a balance.
    [[nodiscard]] inline double to_emeralds(Money grains)
    {
        return static_cast<double>(grains) / static_cast<double>(kGrainsPerEmerald);
    }
}
