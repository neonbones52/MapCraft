#include "game/economy/income.h"

#include "game/calendar.h"
#include "game/countries.h"

namespace mc
{
    namespace
    {
        void add(ProvinceIncome* sum, const ProvinceIncome& income)
        {
            sum->tax += income.tax;
            sum->production += income.production;
            sum->gold += income.gold;
            sum->census += income.census;
        }
    }

    double province_volume(const WorldState& state, const EconomyDefines& defines, std::uint32_t p)
    {
        const Provinces& provinces = state.provinces;
        return defines.production_base +
               static_cast<double>(provinces.population[p]) / static_cast<double>(defines.population_per_unit) +
               static_cast<double>(provinces.base_tax[p]) / static_cast<double>(defines.base_tax_per_unit);
    }

    ProvinceIncome province_income(const WorldState& state, const Content& content, std::uint32_t p,
                                   std::uint32_t seat)
    {
        const Provinces& provinces = state.provinces;
        const std::uint16_t owner = provinces.owner[p];
        if (owner == 0 || provinces.controller[p] != owner) {
            return {};
        }
        const EconomyDefines& defines = content.economy;
        const auto months = static_cast<double>(kMonthsPerYear);
        const double base = static_cast<double>(provinces.base_tax[p]) +
                            (p == seat ? defines.capital_tax : 0.0);
        const double census = provinces.has_core(p, owner) ? defines.census_core : defines.census_no_core;
        ProvinceIncome income{ base / months, 0.0, 0.0, base * census };
        const std::uint8_t good = provinces.good[p];
        if (good != 0) {
            const double units = province_volume(state, defines, p) / months;
            if (good == content.money_good) {
                income.gold = units;
            } else {
                income.production = units * static_cast<double>(content.goods_price[good]);
            }
        }
        return income;
    }

    void world_income(const WorldState& state, const Content& content, WorldIncome* out)
    {
        const std::size_t countries = state.countries.count;
        out->seat.assign(countries, 0);
        out->sum.assign(countries, ProvinceIncome{});
        for (std::size_t c = 1; c < countries; ++c) {
            out->seat[c] = seat(state, static_cast<std::uint16_t>(c));
        }
        const Provinces& provinces = state.provinces;
        for (std::uint32_t p = 1; p < provinces.count; ++p) {
            const std::uint16_t owner = provinces.owner[p];
            if (owner == 0) {
                continue;
            }
            add(&out->sum[owner], province_income(state, content, p, out->seat[owner]));
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
            if (provinces.owner[p] == country) {
                add(&sum, province_income(state, content, p, at));
            }
        }
        return sum;
    }
}
