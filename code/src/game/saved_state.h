#pragma once

#include <span>
#include <string_view>
#include <type_traits>

#include "game/game_data.h"

// The saved part of the world (docs/GAMEDATA.md, "Метки колонок"): every column and number of WorldState that is the
// state, in one fixed order, each under a stable name. mapcraft_sim's dump and checksum walk exactly this list, and
// so will the save. The columns come from the tables' column lists (game/game_data.h), in their order; what is not a
// column of a list is visited by hand. Sizes (counts, strides) are not listed: they follow from the content and the
// map. Content and the cache are never saved.

namespace mc
{
    // Calls visit(name, span) for each saved column (a span over the whole column, padding included) and number (a
    // span of one); `State` is WorldState or const WorldState.
    template <class State, class Visit>
        requires std::is_same_v<std::remove_const_t<State>, WorldState>
    void for_each_saved(State& state, Visit&& visit)
    {
#define MC_VISIT_SAVED(table, type, name, start) \
    visit(std::string_view{ #table "." #name }, std::span(state.table.name));
        MC_PROVINCE_COLUMNS(MC_VISIT_SAVED)
        visit(std::string_view{ "provinces.cores" }, std::span(state.provinces.cores));
        MC_REGION_COLUMNS(MC_VISIT_SAVED)
        MC_BALANCE_COLUMNS(MC_VISIT_SAVED)
        MC_COUNTRY_COLUMNS(MC_VISIT_SAVED)
#undef MC_VISIT_SAVED
    }

    // A tripwire for what is not a column of a list, not a rule: a list's column passes it by itself, a new field
    // written by hand changes the number. Such a field must be labelled (saved, content or cache, game/game_data.h)
    // and, if saved, visited above before the number is updated. By hand now: the sizes (Provinces::count and
    // core_words, Regions::count, CountryStates::count) and the cores, 48 bytes with their padding.
#define MC_COUNT_COLUMN(table, type, name, start) +1
    inline constexpr std::size_t kListedColumns = 0
        MC_PROVINCE_COLUMNS(MC_COUNT_COLUMN) MC_REGION_COLUMNS(MC_COUNT_COLUMN)
        MC_BALANCE_COLUMNS(MC_COUNT_COLUMN) MC_COUNTRY_COLUMNS(MC_COUNT_COLUMN);
#undef MC_COUNT_COLUMN
    static_assert(sizeof(Column<std::uint8_t>) == sizeof(Column<Money>), "a list's columns differ in size");
    static_assert(sizeof(WorldState) == 48 + kListedColumns * sizeof(Column<std::uint8_t>),
                  "WorldState has a new field outside the column lists: label it and visit it in for_each_saved");
}
