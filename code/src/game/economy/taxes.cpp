#include "game/economy/taxes.h"

#include "game/calendar.h"
#include "game/countries.h"

namespace mc
{
    ProvinceIncome province_income(const WorldState& state, const EconomyDefines& defines, std::uint32_t p,
                                   std::uint32_t seat)
    {
        const Provinces& provinces = state.provinces;
        const std::uint16_t owner = provinces.owner[p];
        if (owner == 0 || provinces.controller[p] != owner) {
            return {};
        }
        const double base = static_cast<double>(provinces.base_tax[p]) +
                            (p == seat ? fx_to_double(defines.capital_tax) : 0.0);
        const Fixed census = provinces.has_core(p, owner) ? defines.census_core : defines.census_no_core;
        return { base / static_cast<double>(kMonthsPerYear), base * fx_to_double(census) };
    }

    void world_income(const WorldState& state, const Content& content, WorldIncome* out)
    {
        const std::size_t countries = state.countries.count;
        out->seat.assign(countries, 0);
        out->tax.assign(countries, 0.0);
        out->census.assign(countries, 0.0);
        for (std::size_t c = 1; c < countries; ++c) {
            out->seat[c] = seat(state, static_cast<std::uint16_t>(c));
        }
        const Provinces& provinces = state.provinces;
        for (std::uint32_t p = 1; p < provinces.count; ++p) {
            const std::uint16_t owner = provinces.owner[p];
            if (owner == 0) {
                continue;
            }
            const ProvinceIncome income = province_income(state, content.economy, p, out->seat[owner]);
            out->tax[owner] += income.tax;
            out->census[owner] += income.census;
        }
    }

    ProvinceIncome country_income(const WorldState& state, const Content& content, std::uint16_t country)
    {
        ProvinceIncome sum;
        if (country == 0 || country >= state.countries.count) {
            return sum;
        }
        const std::uint32_t at = seat(state, country);
        const Provinces& provinces = state.provinces;
        for (std::uint32_t p = 1; p < provinces.count; ++p) {
            if (provinces.owner[p] != country) {
                continue;
            }
            const ProvinceIncome income = province_income(state, content.economy, p, at);
            sum.tax += income.tax;
            sum.census += income.census;
        }
        return sum;
    }
}
