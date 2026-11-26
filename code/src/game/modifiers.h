#pragma once

#include <cstddef>
#include <span>
#include <string_view>

#include "core/fixed.h"
#include "game/game_data.h"

// The modifiers (docs/GAMEDATA.md, "Модификаторы"): source -> modifier -> value. The sources are the bundles of
// data/common/modifiers.csv (Content::modifiers): the base bundle every country has and the ones in its set
// (CountryStates::modifiers, saved). Their values add up into each country's sums (CountryModifiers, the cache); the
// rules read the sums only through the formulas below. All in Fixed (core/fixed.h), as EU4. The list of modifiers:
// MC_COUNTRY_MODIFIERS (game/game_data.h).

namespace mc
{
    // Every country's sums (`out`, resized to the countries' padded count) from the bundles' values: the base
    // bundle's, then each bundle in the country's set, in the bundles' order. Integer additions, exact; a pass over
    // the countries per bundle, no branches inside.
    void sum_country_modifiers(const ModifierBundles& bundles, const CountryStates& countries, CountryModifiers* out);

    // The formulas over the sums: what the rules (the battle, game/combat.h) and the UI read.
    // Discipline, 1 = 100%: the damage the country's regiments deal and take.
    [[nodiscard]] inline Fixed discipline(const CountryModifiers& sums, std::size_t country)
    {
        return sums.discipline[country];
    }
    // The maximum morale of the country's regiments: base_morale × (1 + land_morale), as in EU4.
    [[nodiscard]] inline Fixed max_morale(const CountryModifiers& sums, std::size_t country)
    {
        return fx_mul(sums.base_morale[country], kFixedOne + sums.land_morale[country]);
    }
    // The tactics the country's regiments are shot at with: military_tactics × discipline (EU4's
    // CCountry::GetMilitaryTactics); a shot's damage is divided by it.
    [[nodiscard]] inline Fixed tactics(const CountryModifiers& sums, std::size_t country)
    {
        return fx_mul(sums.military_tactics[country], sums.discipline[country]);
    }
    // The width of the country's front line, whole regiments.
    [[nodiscard]] inline int combat_width(const CountryModifiers& sums, std::size_t country)
    {
        return fx_to_int(sums.combat_width[country]);
    }
    // The weapons' damage of a category (infantry 0, cavalry 1) in a phase (fire 0, shock 1); 0 for artillery,
    // which has no line to shoot from yet.
    [[nodiscard]] inline Fixed weapon(const CountryModifiers& sums, std::size_t country, RegimentCategory category,
                                      int phase)
    {
        switch (category) {
        case RegimentCategory::infantry:
            return phase == 0 ? sums.infantry_fire[country] : sums.infantry_shock[country];
        case RegimentCategory::cavalry:
            return phase == 0 ? sums.cavalry_fire[country] : sums.cavalry_shock[country];
        case RegimentCategory::artillery:
            break;
        }
        return 0;
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
