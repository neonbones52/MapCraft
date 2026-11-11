#include "game/countries.h"

namespace mc
{
    bool owns_land(const WorldState& state, std::uint16_t country)
    {
        if (country == 0 || country >= state.countries.count) {
            return false;
        }
        for (std::uint32_t id = 1; id < state.provinces.count; ++id) {
            if (state.provinces.owner[id] == country) {
                return true;
            }
        }
        return false;
    }

    std::uint32_t seat(const WorldState& state, std::uint16_t country)
    {
        if (country == 0 || country >= state.countries.count) {
            return 0;
        }
        const std::uint16_t capital = state.countries.capital[country];
        if (capital != 0 && state.provinces.owner[capital] == country) {
            return capital;
        }
        std::uint32_t best = 0;
        float best_development = -1.0f;
        for (std::uint32_t id = 1; id < state.provinces.count; ++id) {
            if (state.provinces.owner[id] != country) {
                continue;
            }
            const float d = state.provinces.tax[id] + state.provinces.production[id] + state.provinces.manpower[id];
            if (d > best_development) {
                best_development = d;
                best = id;
            }
        }
        return best;
    }

    CountryStats country_stats(const WorldState& state, std::uint16_t country)
    {
        CountryStats s;
        const Provinces& p = state.provinces;
        for (std::uint32_t id = 1; id < p.count; ++id) {
            s.cores += p.has_core(id, country) ? 1 : 0;
            if (p.owner[id] != country) {
                continue;
            }
            ++s.provinces;
            s.tax += p.tax[id];
            s.production += p.production[id];
            s.manpower += p.manpower[id];
        }
        return s;
    }
}
