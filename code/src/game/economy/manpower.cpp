#include "game/economy/manpower.h"

#include <algorithm>
#include <cmath>

#include "game/calendar.h"

namespace mc
{
    namespace
    {
        // What province `p` adds to its owner's maximum: nothing if occupied.
        double province_manpower(const Provinces& provinces, const EconomyDefines& defines, std::uint32_t p)
        {
            const std::uint16_t owner = provinces.owner[p];
            if (owner == 0 || provinces.controller[p] != owner) {
                return 0.0;
            }
            const double share = provinces.has_core(p, owner) ? defines.manpower_core : defines.manpower_no_core;
            return static_cast<double>(provinces.population[p]) * share;
        }

        std::int32_t whole_men(double men)
        {
            return men > 0.0 ? static_cast<std::int32_t>(std::floor(men)) : 0;
        }
    }

    std::int32_t max_manpower(const WorldState& state, const Content& content, std::uint16_t country)
    {
        if (country == 0 || country >= state.countries.count) {
            return 0;
        }
        const EconomyDefines& defines = content.economy;
        double sum = static_cast<double>(defines.manpower_base);
        const Provinces& provinces = state.provinces;
        for (std::uint32_t p = 1; p < provinces.count; ++p) {
            if (provinces.owner[p] == country) {
                sum += province_manpower(provinces, defines, p);
            }
        }
        return whole_men(sum);
    }

    std::int32_t monthly_manpower(const EconomyDefines& defines, std::int32_t maximum)
    {
        const double months = static_cast<double>(defines.manpower_recovery_years) * kMonthsPerYear;
        return whole_men(static_cast<double>(maximum) / months);
    }

    void world_max_manpower(const WorldState& state, const Content& content, WorldManpower* out)
    {
        const std::size_t countries = state.countries.count;
        const EconomyDefines& defines = content.economy;
        out->sum.assign(countries, static_cast<double>(defines.manpower_base));
        out->maximum.assign(countries, 0);
        const Provinces& provinces = state.provinces;
        for (std::uint32_t p = 1; p < provinces.count; ++p) {
            out->sum[provinces.owner[p]] += province_manpower(provinces, defines, p);
        }
        for (std::size_t c = 1; c < countries; ++c) {
            out->maximum[c] = whole_men(out->sum[c]);
        }
    }

    void fill_manpower(WorldState& state, const Content& content)
    {
        WorldManpower work;
        world_max_manpower(state, content, &work);
        CountryStates& countries = state.countries;
        std::fill(countries.manpower.begin(), countries.manpower.end(), 0);
        for (std::size_t c = 1; c < countries.count; ++c) {
            countries.manpower[c] = whole_men(static_cast<double>(work.maximum[c]) * content.economy.manpower_start);
        }
    }

    void Manpower::month(WorldState& state, const Content& content)
    {
        world_max_manpower(state, content, &work_);
        CountryStates& countries = state.countries;
        for (std::size_t c = 1; c < countries.count; ++c) {
            const std::int32_t maximum = work_.maximum[c];
            const std::int32_t pool = countries.manpower[c];
            countries.manpower[c] =
                pool >= maximum ? maximum
                                : std::min(maximum, pool + monthly_manpower(content.economy, maximum));
        }
    }
}
