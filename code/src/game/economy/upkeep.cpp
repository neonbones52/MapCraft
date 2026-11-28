#include "game/economy/upkeep.h"

#include <algorithm>
#include <cmath>

#include "game/armies.h"
#include "game/countries.h"
#include "game/modifiers.h"

namespace mc
{
    namespace
    {
        // The force limit from the base_tax the country owns and whether it has a seat: rounded down, at least 0.
        std::uint32_t limit_of(const EconomyDefines& defines, double base_tax, bool seated)
        {
            const double limit = defines.force_limit_base +
                                 defines.force_limit_per_base_tax * (base_tax + (seated ? defines.capital_tax : 0.0));
            return limit > 0.0 ? static_cast<std::uint32_t>(std::floor(limit)) : 0u;
        }

        // One regiment's base upkeep a month.
        double regiment_upkeep(const Content& content, const Regiments& regiments, std::uint32_t r)
        {
            return static_cast<double>(regiments.strength[r]) / 1000.0 *
                   static_cast<double>(content.regiment_cost[regiments.type[r]]) * content.economy.land_maintenance;
        }
    }

    std::uint32_t force_limit(const WorldState& state, const Content& content, std::uint16_t country)
    {
        double base_tax = 0.0;
        const Provinces& provinces = state.provinces;
        for (std::uint32_t p = 1; p < provinces.count; ++p) {
            if (provinces.owner[p] == country) {
                base_tax += static_cast<double>(provinces.base_tax[p]);
            }
        }
        return limit_of(content.economy, base_tax, seat(state, country) != 0);
    }

    double land_upkeep(const EconomyDefines& defines, double base, std::uint32_t regiments, std::uint32_t limit,
                       std::uint8_t maintenance, float inflation)
    {
        // No limit at all: every regiment is one over it.
        const double whole = static_cast<double>(limit);
        const double excess = static_cast<double>(regiments) - whole;
        const double over = excess <= 0.0 ? 0.0
                            : limit == 0  ? defines.force_limit_extra_cost * excess
                                          : defines.force_limit_extra_cost * excess / whole;
        return base * static_cast<double>(maintenance) / 100.0 * (1.0 + over) *
               (1.0 + static_cast<double>(inflation) / 100.0);
    }

    ArmyUpkeep army_upkeep(const WorldState& state, const Content& content, std::uint16_t country)
    {
        ArmyUpkeep out;
        if (country == 0 || country >= state.countries.count) {
            return out;
        }
        const Regiments& regiments = state.regiments;
        for_each_alive(regiments.alive, [&](std::uint32_t r) {
            if (state.armies.owner[regiments.army[r]] == country) {
                out.base += regiment_upkeep(content, regiments, r);
                ++out.regiments;
            }
        });
        out.limit = force_limit(state, content, country);
        out.upkeep = land_upkeep(content.economy, out.base, out.regiments, out.limit,
                                 state.countries.maintenance[country], state.countries.inflation[country]);
        return out;
    }

    void world_upkeep(const WorldState& state, const Content& content, WorldUpkeep* out)
    {
        const std::size_t countries = state.countries.count;
        out->base_tax.assign(countries, 0.0);
        out->limit.assign(countries, 0);
        out->base.assign(countries, 0.0);
        out->regiments.assign(countries, 0);
        const Provinces& provinces = state.provinces;
        for (std::uint32_t p = 1; p < provinces.count; ++p) {
            out->base_tax[provinces.owner[p]] += static_cast<double>(provinces.base_tax[p]);
        }
        for (std::size_t c = 1; c < countries; ++c) {
            out->limit[c] =
                limit_of(content.economy, out->base_tax[c], seat(state, static_cast<std::uint16_t>(c)) != 0);
        }
        const Regiments& regiments = state.regiments;
        for_each_alive(regiments.alive, [&](std::uint32_t r) {
            const std::uint16_t owner = state.armies.owner[regiments.army[r]];
            out->base[owner] += regiment_upkeep(content, regiments, r);
            ++out->regiments[owner];
        });
    }

    Fixed morale_ceiling(const CountryModifiers& sums, const CountryStates& countries, const EconomyDefines& defines,
                         std::uint16_t country)
    {
        if (country == 0 || country >= countries.count || countries.maintenance[country] >= 100) {
            return kNoMoraleCeiling;
        }
        const int lowest = defines.maintenance_min;
        const int level = std::max<int>(countries.maintenance[country], lowest);
        const Fixed share = defines.min_maintenance_morale +
                            static_cast<Fixed>(std::int64_t{ kFixedOne - defines.min_maintenance_morale } *
                                               (level - lowest) / (100 - lowest));
        return fx_mul(max_morale(sums, country), share);
    }

    const char* why_no_maintenance(const WorldState& state, const EconomyDefines& defines, std::uint16_t country,
                                   std::uint8_t level)
    {
        if (country == 0 || country >= state.countries.count) {
            return "no such country";
        }
        if (level < defines.maintenance_min || level > 100) {
            return "not within the slider's range";
        }
        return nullptr;
    }
}
