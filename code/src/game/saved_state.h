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
    // Calls visit(name, span) for each saved column (a span over the whole column, padding included; over the used
    // slots of a slot table) and number (a span of one); `State` is WorldState or const WorldState.
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
        visit(std::string_view{ "countries.modifiers" }, std::span(state.countries.modifiers));
#undef MC_VISIT_SAVED
        // Slot tables: only the used part, the slots up to the last live one's mask word (the rest is 0, freed slots
        // are cleared), so the dump and the checksum do not walk tens of thousands of empty slots every day.
#define MC_VISIT_SLOTS(table, type, name, start) \
    visit(std::string_view{ #table "." #name }, std::span(state.table.name).first(used));
        {
            const std::size_t used = used_slots(state.armies.alive);
            MC_ARMY_COLUMNS(MC_VISIT_SLOTS)
            visit(std::string_view{ "armies.alive" }, std::span(state.armies.alive).first(used / 64));
            visit(std::string_view{ "armies.retreating" }, std::span(state.armies.retreating).first(used / 64));
        }
        {
            const std::size_t used = used_slots(state.regiments.alive);
            MC_REGIMENT_COLUMNS(MC_VISIT_SLOTS)
            visit(std::string_view{ "regiments.alive" }, std::span(state.regiments.alive).first(used / 64));
        }
        {
            const std::size_t used = used_slots(state.leaders.alive);
            MC_LEADER_COLUMNS(MC_VISIT_SLOTS)
            visit(std::string_view{ "leaders.alive" }, std::span(state.leaders.alive).first(used / 64));
        }
        {
            // A structure, not columns (game/game_data.h, Battle): its bytes, field by field in the dump.
            const std::size_t used = used_slots(state.battles.alive);
            visit(std::string_view{ "battles" }, std::span(state.battles.list).first(used));
            visit(std::string_view{ "battles.alive" }, std::span(state.battles.alive).first(used / 64));
        }
        {
            const std::size_t used = used_slots(state.wars.alive);
            MC_WAR_COLUMNS(MC_VISIT_SLOTS)
            visit(std::string_view{ "wars.alive" }, std::span(state.wars.alive).first(used / 64));
        }
        {
            const std::size_t used = used_slots(state.sieges.alive);
            MC_SIEGE_COLUMNS(MC_VISIT_SLOTS)
            visit(std::string_view{ "sieges.alive" }, std::span(state.sieges.alive).first(used / 64));
        }
#undef MC_VISIT_SLOTS
    }

    // A tripwire for what is not a column of a list, not a rule: a list's column passes it by itself, a new field
    // written by hand changes the number. Such a field must be labelled (saved, content or cache, game/game_data.h)
    // and, if saved, visited above before the number is updated. By hand now: the sizes (Provinces::count and
    // core_words, Regions::count, CountryStates::count and modifier_words), the cores, the countries' modifier
    // bundles, the six slot masks, the armies' retreating mask and the battles, 272 bytes with their padding.
#define MC_COUNT_COLUMN(table, type, name, start) +1
    inline constexpr std::size_t kListedColumns = 0
        MC_PROVINCE_COLUMNS(MC_COUNT_COLUMN) MC_REGION_COLUMNS(MC_COUNT_COLUMN)
        MC_BALANCE_COLUMNS(MC_COUNT_COLUMN) MC_COUNTRY_COLUMNS(MC_COUNT_COLUMN)
        MC_ARMY_COLUMNS(MC_COUNT_COLUMN) MC_REGIMENT_COLUMNS(MC_COUNT_COLUMN) MC_LEADER_COLUMNS(MC_COUNT_COLUMN)
        MC_WAR_COLUMNS(MC_COUNT_COLUMN) MC_SIEGE_COLUMNS(MC_COUNT_COLUMN);
#undef MC_COUNT_COLUMN
    static_assert(sizeof(Column<std::uint8_t>) == sizeof(Column<Money>), "a list's columns differ in size");
    static_assert(sizeof(WorldState) == 272 + kListedColumns * sizeof(Column<std::uint8_t>),
                  "WorldState has a new field outside the column lists: label it and visit it in for_each_saved");
}
