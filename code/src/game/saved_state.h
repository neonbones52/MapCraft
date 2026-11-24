#pragma once

#include <array>
#include <span>
#include <string_view>
#include <type_traits>

#include "game/game_data.h"

// The saved part of the world (docs/GAMEDATA.md, "Метки колонок"): every column and number of WorldState that is the
// state, in one fixed order, each under a stable name. mapcraft_sim's dump and checksum walk exactly this list, and
// so will the save. Sizes (count, stride, types, goods) are not listed: they follow from the content and the map.
// Content and the cache (EconomyCache) are never saved.

namespace mc
{
    inline constexpr std::array<std::string_view, kNeedTiers> kSavedFilled = {
        "pops.filled.essential", "pops.filled.everyday", "pops.filled.luxury" };

    // Calls visit(name, span) for each saved column (a span over the whole column, padding included) and number (a
    // span of one); `State` is WorldState or const WorldState.
    template <class State, class Visit>
        requires std::is_same_v<std::remove_const_t<State>, WorldState>
    void for_each_saved(State& state, Visit&& visit)
    {
        visit(std::string_view{ "provinces.owner" }, std::span(state.provinces.owner));
        visit(std::string_view{ "provinces.controller" }, std::span(state.provinces.controller));
        visit(std::string_view{ "provinces.terrain" }, std::span(state.provinces.terrain));
        visit(std::string_view{ "provinces.rgo" }, std::span(state.provinces.rgo));
        visit(std::string_view{ "provinces.cores" }, std::span(state.provinces.cores));
        visit(std::string_view{ "regions.market" }, std::span(state.regions.market));
        visit(std::string_view{ "pops.size" }, std::span(state.pops.size));
        for (std::size_t tier = 0; tier < kNeedTiers; ++tier) {
            visit(kSavedFilled[tier], std::span(state.pops.filled[tier]));
        }
        visit(std::string_view{ "stock.amount" }, std::span(state.stock.amount));
        visit(std::string_view{ "prices.price" }, std::span(state.prices.price));
        visit(std::string_view{ "money.cash" }, std::span(state.money.cash));
        visit(std::string_view{ "money.start" }, std::span(&state.money.start, 1));
        visit(std::string_view{ "money.minted" }, std::span(&state.money.minted, 1));
        visit(std::string_view{ "money.flows" }, std::span(state.money.flows));
        visit(std::string_view{ "countries.capital" }, std::span(state.countries.capital));
    }

    // A tripwire, not a rule: a new field of WorldState changes its size, and then it must be labelled (saved, content
    // or cache, game/game_data.h) and, if saved, listed above before this number is updated.
    static_assert(sizeof(WorldState) == 456, "WorldState changed: list its new saved fields in for_each_saved");
}
