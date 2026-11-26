#pragma once

#include <cstddef>
#include <span>
#include <string_view>

#include "game/game_data.h"

// The modifiers (docs/SIMULATION.md, "Модификаторы"): source -> modifier -> value. The sources are the bundles of
// data/common/modifiers.csv (Content::modifiers): the base bundle every country has and the ones in its set
// (CountryStates::modifiers, saved). Their values add up into each country's sums (CountryModifiers, the cache); the
// rules read the sums only through the formulas below. The list of modifiers: MC_COUNTRY_MODIFIERS (game/game_data.h).

namespace mc
{
    // Every country's sums (`out`, resized to the countries' padded count) from the bundles' values: the base
    // bundle's, then each bundle in the country's set, in the bundles' order. One fixed order of additions, so the
    // sums are the same bits on every machine; a pass over the countries per bundle, no branches inside.
    void sum_country_modifiers(const ModifierBundles& bundles, const CountryStates& countries, CountryModifiers* out);

    // The formulas over the sums: what the rules (the battle, M6) and the UI read.
    // Discipline, 1 = 100%: the damage the country's regiments deal and take.
    [[nodiscard]] inline float discipline(const CountryModifiers& sums, std::size_t country)
    {
        return sums.discipline[country];
    }
    // The maximum morale of the country's regiments: base_morale × (1 + land_morale), as in EU4.
    [[nodiscard]] inline float max_morale(const CountryModifiers& sums, std::size_t country)
    {
        return sums.base_morale[country] * (1.0f + sums.land_morale[country]);
    }

    // visit(name, span) for every column of the cache, over its used part (countries 0..count-1), in one fixed order
    // under stable names: mapcraft_sim's cache dump.
    template <class Visit>
    void for_each_cached(const Cache& cache, Visit&& visit)
    {
        const CountryModifiers& sums = cache.country_modifiers;
#define MC_VISIT_SUM(name) visit(std::string_view{ "country_modifiers." #name }, std::span(sums.name).first(sums.count));
        MC_COUNTRY_MODIFIERS(MC_VISIT_SUM)
#undef MC_VISIT_SUM
    }
}
