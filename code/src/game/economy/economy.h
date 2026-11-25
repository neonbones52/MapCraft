#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <vector>

#include "core/money.h"
#include "game/game_data.h"

// The economy's day: its phases, in the one order written in economy.cpp (docs/GAMEDATA.md, "Экономика дня").

namespace mc
{
    // Cache (the labels: game/game_data.h). What the day needs besides the state, kept between days (docs/GAMEDATA.md,
    // "Экономика дня"): no state of its own, so never saved; prepare_economy rebuilds it from the content and the
    // state, deterministically.
    //   index    what changes only with the data: the markets as flat lists (provinces and pops, market after
    //            market, each by id)
    //   scratch  columns one clearing fills and the next reuses: nothing is allocated once the day runs; the day's
    //            supply and demand per good and market (the same layout as MarketPrices) for the price pass
    struct EconomyCache
    {
        // Market m: provinces [province_first[m], province_first[m + 1]), pops [pop_first[m], pop_first[m + 1]).
        std::vector<std::uint32_t> province_first;
        std::vector<std::uint32_t> province;
        std::vector<std::uint32_t> pop_first;
        std::vector<std::uint32_t> pop_account;     // the pop's place in Pops, its account in Balances
        std::vector<std::uint8_t> pop_type;
        int money_good = -1;                        // Content::goods index of kMoneyGood

        // The day, per good and market (g * MarketPrices::stride + m): stock on the market before it trades, and what
        // its pops wanted of the good with the money they have (unmet demand added to goods that ran out).
        Column<float> market_supply;
        Column<float> market_demand;

        // Scratch: per good of the tier (its market price, order cheapest per unit of the tier first, whether the
        // walk used it up), per pop of the market, per payee.
        std::vector<double> unit;
        std::vector<std::uint8_t> order;
        std::vector<std::uint8_t> used_up;
        std::vector<double> supply;
        std::vector<double> taken;
        std::vector<double> want;
        std::vector<double> fill;
        std::vector<Money> pay;
        std::vector<std::uint32_t> payee;
        std::vector<double> weight;
    };

    // Builds the index (after loading, and whenever regions change market).
    void prepare_economy(const Content& content, const WorldState& state, EconomyCache* cache);

    // One day: mining, production, every market clearing each tier of needs, then the prices move.
    void economy_day(const Content& content, WorldState* state, EconomyCache* cache);

    // How much of a tier of needs the province's pops got on the last day, weighted by people: 0..1, or -1 when no pop
    // of the province needs that tier (UI).
    [[nodiscard]] float needs_filled(const Content& content, const WorldState& state, std::uint32_t province,
                                     std::size_t tier);
}
