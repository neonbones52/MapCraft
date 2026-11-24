#include "game/economy/economy.h"

#include <algorithm>
#include <cmath>

#include "game/economy/ledger.h"

// Phase -> what it writes (docs/ECONOMY.md, "Код"); the order is economy_day's, at the end of this file.
//   mine            Balances::cash (owners mint, workers' wages), minted, flows[mine, wages]
//   produce         Stock::amount
//   measure_supply  EconomyCache::market_supply, market_demand (cleared)
//   clear_tier      Pops::filled, Stock::amount, Balances::cash, flows[sales], EconomyCache::market_demand, scratch
//   move_prices     MarketPrices::price

namespace mc
{
    namespace
    {
        std::uint32_t account(const Pops& pops, std::size_t type, std::uint32_t province)
        {
            return static_cast<std::uint32_t>(type * pops.stride + province);
        }

        // Mining: an RGO whose outputs hold the money good mints, every day, workers x its amount per worker into
        // the province's pop of its owners' type, and they pay its workers their share (rgo_types.csv `wages`).
        void mine(const Content& content, WorldState* state, int money_good)
        {
            const RgoTypes& rgos = content.rgo_types;
            const Pops& pops = state->pops;
            for (std::uint32_t p = 1; p < state->provinces.count; ++p) {
                const std::uint8_t rgo = state->provinces.rgo[p];
                if (rgo == 0) {
                    continue;
                }
                for (std::uint32_t o = rgos.output_first[rgo]; o < rgos.output_first[rgo + 1]; ++o) {
                    if (rgos.output_good[o] != money_good) {
                        continue;
                    }
                    const double workers = pops.at(rgos.workers[rgo], p);
                    const Money amount = to_grains(workers * static_cast<double>(rgos.output_amount[o]));
                    const std::uint32_t owners = account(pops, rgos.owners[rgo], p);
                    ledger::mint(&state->money, owners, amount, Flow::mine);
                    const auto wage = static_cast<Money>(std::floor(static_cast<double>(amount) * rgos.wages[rgo]));
                    ledger::transfer(&state->money, owners, account(pops, rgos.workers[rgo], p), wage, Flow::wages);
                }
            }
        }

        // Production: every RGO adds workers x its amount per worker of each of its goods (but money) to its stock.
        void produce(const Content& content, WorldState* state, int money_good)
        {
            const RgoTypes& rgos = content.rgo_types;
            for (std::uint32_t p = 1; p < state->provinces.count; ++p) {
                const std::uint8_t rgo = state->provinces.rgo[p];
                if (rgo == 0) {
                    continue;
                }
                const float workers = state->pops.at(rgos.workers[rgo], p);
                for (std::uint32_t o = rgos.output_first[rgo]; o < rgos.output_first[rgo + 1]; ++o) {
                    if (rgos.output_good[o] != money_good) {
                        state->stock.at(rgos.output_good[o], p) += workers * rgos.output_amount[o];
                    }
                }
            }
        }

        // Buys `fill` units of a tier from its goods in the order of c->order, cheapest first, out of c->supply at
        // c->unit: what each good gives goes to c->taken, c->used_up marks the goods it took whole; returns the fill
        // got and the emeralds it costs in `spent`.
        double walk(const Content& content, std::size_t tier, EconomyCache* c, double fill, double* spent)
        {
            const std::vector<float>& weights = content.needs.weight[tier];
            std::fill(c->taken.begin(), c->taken.end(), 0.0);
            std::fill(c->used_up.begin(), c->used_up.end(), std::uint8_t{ 0 });
            *spent = 0.0;
            double got = 0.0;
            for (const std::uint8_t i : c->order) {
                const double whole = c->supply[i] * weights[i];
                const double take = std::min(whole, fill - got);
                c->used_up[i] = take >= whole ? 1 : 0;
                if (take <= 0.0) {
                    continue;
                }
                c->taken[i] = take / weights[i];
                *spent += c->taken[i] * c->unit[i];
                got += take;
            }
            return got;
        }

        // One tier of needs on one market (docs/GAMEDATA.md, "Экономика дня"): the market buys its pops' demand of
        // the tier from the goods that fill it, cheapest per unit of the tier first, at the market's prices; short
        // supply gives every pop the same share, min(1, S/D); a pop short of cash buys what its cash pays for. Each
        // RGO sells the same share of its stock of a good; the money goes to its workers and owners by its wage
        // share. What the pops wanted of each good with their money goes to the day's demand for the price pass.
        // Every step is a pass over the market's columns in the cache.
        void clear_tier(const Content& content, WorldState* state, EconomyCache* c, std::size_t market,
                        std::size_t tier)
        {
            const std::vector<std::uint8_t>& goods = content.needs.good[tier];
            const std::vector<float>& weights = content.needs.weight[tier];
            const std::size_t n = goods.size();
            const std::uint32_t p0 = c->province_first[market];
            const std::uint32_t p1 = c->province_first[market + 1];
            const std::uint32_t e0 = c->pop_first[market];
            const std::uint32_t e1 = c->pop_first[market + 1];
            const std::size_t pops = e1 - e0;
            const std::size_t mstride = state->prices.stride;
            Column<float>& filled = state->pops.filled[tier];

            // The tier's goods on this market: price, supply, order cheapest per unit of the tier (then by good).
            c->unit.resize(n);
            c->supply.assign(n, 0.0);
            c->taken.assign(n, 0.0);
            c->used_up.assign(n, 0);
            c->order.resize(n);
            for (std::size_t i = 0; i < n; ++i) {
                c->unit[i] = state->prices.at(goods[i], market);
                const float* row = &state->stock.amount[goods[i] * state->stock.stride];
                for (std::uint32_t k = p0; k < p1; ++k) {
                    c->supply[i] += row[c->province[k]];
                }
                c->order[i] = static_cast<std::uint8_t>(i);
            }
            std::sort(c->order.begin(), c->order.end(), [&](std::uint8_t a, std::uint8_t b) {
                const double ca = c->unit[a] / weights[a];
                const double cb = c->unit[b] / weights[b];
                return ca != cb ? ca < cb : goods[a] < goods[b];
            });

            // Demand, per pop and in all; what of it the pops' money reaches at the cheapest price on offer.
            c->want.resize(pops);
            double demand = 0.0;
            for (std::size_t e = 0; e < pops; ++e) {
                c->want[e] = static_cast<double>(content.pop_types.need[c->pop_type[e0 + e]][tier]) *
                             state->pops.size[c->pop_account[e0 + e]];
                demand += c->want[e];
            }
            if (demand <= 0.0) {
                for (std::size_t e = 0; e < pops; ++e) {
                    filled[c->pop_account[e0 + e]] = 0.0f;
                }
                return;
            }
            double cheapest = 0.0;
            for (const std::uint8_t i : c->order) {
                if (c->supply[i] > 0.0) {
                    cheapest = c->unit[i] / weights[i];
                    break;
                }
            }
            double spent = 0.0;
            if (cheapest > 0.0) {
                double affordable = 0.0;
                for (std::size_t e = 0; e < pops; ++e) {
                    const double cash = to_emeralds(state->money.cash[c->pop_account[e0 + e]]);
                    affordable += std::min(c->want[e], cash / cheapest);
                }
                // What the market could not give is shared among the goods that ran out, by how much of the tier
                // each held (counted once in all, not once per good).
                const double unmet = affordable - walk(content, tier, c, affordable, &spent);
                double held = 0.0;
                for (std::size_t i = 0; i < n; ++i) {
                    held += c->used_up[i] ? c->supply[i] * weights[i] : 0.0;
                }
                for (std::size_t i = 0; i < n; ++i) {
                    const double part = c->used_up[i] && held > 0.0 ? c->supply[i] * weights[i] / held : 0.0;
                    const double extra = unmet * part / weights[i];
                    c->market_demand[goods[i] * mstride + market] += static_cast<float>(c->taken[i] + extra);
                }
            }

            const double got = walk(content, tier, c, demand, &spent);
            if (got <= 0.0) {
                for (std::size_t e = 0; e < pops; ++e) {
                    filled[c->pop_account[e0 + e]] = 0.0f;
                }
                return;
            }
            const double share = got / demand;   // min(1, S/D)
            const double cost = spent / got;     // emeralds per unit of the tier

            // What each pop can pay for, then what that costs bought cheapest first.
            c->fill.resize(pops);
            double bought = 0.0;
            for (std::size_t e = 0; e < pops; ++e) {
                const double cash = to_emeralds(state->money.cash[c->pop_account[e0 + e]]);
                c->fill[e] = std::min(share * c->want[e], cash / cost);
                bought += c->fill[e];
            }
            if (bought <= 0.0) {
                for (std::size_t e = 0; e < pops; ++e) {
                    filled[c->pop_account[e0 + e]] = 0.0f;   // nobody can pay: nothing is sold
                }
                return;
            }
            const double paid = walk(content, tier, c, bought, &spent);
            const double price = paid > 0.0 ? spent / paid : 0.0;
            c->pay.resize(pops);
            for (std::size_t e = 0; e < pops; ++e) {
                const std::uint32_t a = c->pop_account[e0 + e];
                c->pay[e] = std::min(to_grains(c->fill[e] * price), state->money.cash[a]);
                filled[a] = c->want[e] > 0.0 ? static_cast<float>(c->fill[e] / c->want[e]) : 0.0f;
            }

            // Sellers: each RGO the same share of its stock of each good taken; paid by revenue.
            const RgoTypes& rgos = content.rgo_types;
            c->payee.clear();
            c->weight.clear();
            for (std::size_t i = 0; i < n; ++i) {
                if (c->taken[i] <= 0.0 || c->supply[i] <= 0.0) {
                    continue;
                }
                const double part = std::min(c->taken[i] / c->supply[i], 1.0);
                float* row = &state->stock.amount[goods[i] * state->stock.stride];
                for (std::uint32_t k = p0; k < p1; ++k) {
                    const std::uint32_t p = c->province[k];
                    const std::uint8_t rgo = state->provinces.rgo[p];
                    if (row[p] <= 0.0f || rgo == 0) {
                        continue;
                    }
                    const double sold = row[p] * part;
                    row[p] = part >= 1.0 ? 0.0f : static_cast<float>(row[p] - sold);
                    const double wages = rgos.wages[rgo];
                    c->payee.push_back(account(state->pops, rgos.workers[rgo], p));
                    c->weight.push_back(sold * c->unit[i] * wages);
                    c->payee.push_back(account(state->pops, rgos.owners[rgo], p));
                    c->weight.push_back(sold * c->unit[i] * (1.0 - wages));
                }
            }
            ledger::settle(&state->money, { c->pop_account.data() + e0, pops }, c->pay, c->payee, c->weight,
                           Flow::sales);
        }

        // Every market's stock of every good before it trades (rows by good, a column of markets).
        void measure_supply(const Content& content, const WorldState& state, EconomyCache* c)
        {
            const std::size_t mstride = state.prices.stride;
            std::fill(c->market_supply.begin(), c->market_supply.end(), 0.0f);
            std::fill(c->market_demand.begin(), c->market_demand.end(), 0.0f);
            if (c->province_first.size() < 2) {
                return;
            }
            const std::size_t markets = c->province_first.size() - 1;
            for (std::size_t g = 1; g < content.goods.size(); ++g) {
                const float* stock = &state.stock.amount[g * state.stock.stride];
                float* supply = &c->market_supply[g * mstride];
                for (std::size_t m = 1; m < markets; ++m) {
                    float sum = 0.0f;
                    for (std::uint32_t k = c->province_first[m]; k < c->province_first[m + 1]; ++k) {
                        sum += stock[c->province[k]];
                    }
                    supply[m] = sum;
                }
            }
        }

        // Prices: each moves by the day's imbalance (D - S) / max(D, S), at most price_step of itself and not when
        // the imbalance is within price_epsilon, never under price_floor of its start; a good not on a market at
        // all (S = 0) keeps its price there. The money good stays 1. One dense pass per good over its markets.
        void move_prices(const Content& content, WorldState* state, const EconomyCache& c)
        {
            const Defines& d = content.defines;
            const std::size_t mstride = state->prices.stride;
            for (std::size_t g = 1; g < content.goods.size(); ++g) {
                if (static_cast<int>(g) == c.money_good) {
                    continue;
                }
                const float floor = content.goods_price[g] * d.price_floor;
                float* price = &state->prices.price[g * mstride];
                const float* supply = &c.market_supply[g * mstride];
                const float* demand = &c.market_demand[g * mstride];
                for (std::size_t m = 0; m < mstride; ++m) {
                    const float s = supply[m];
                    const float imbalance = s > 0.0f ? (demand[m] - s) / std::max(demand[m], s) : 0.0f;
                    const float step = std::fabs(imbalance) > d.price_epsilon ? d.price_step * imbalance : 0.0f;
                    price[m] = std::max(price[m] * (1.0f + step), floor);
                }
            }
        }
    }

    void prepare_economy(const Content& content, const WorldState& state, EconomyCache* c)
    {
        c->money_good = content.goods.find(kMoneyGood);
        const std::size_t markets = content.markets.size();
        const Pops& pops = state.pops;
        std::vector<std::uint8_t> market_of(state.provinces.count, 0);
        std::vector<std::uint32_t> count(markets + 1, 0);
        for (std::uint32_t p = 1; p < state.provinces.count; ++p) {
            const std::uint16_t region = content.province_region[p];
            market_of[p] = region < state.regions.count ? state.regions.market[region] : 0;
            ++count[market_of[p]];
        }
        // Counting sort by market: provinces by id within each, their pops by province then type.
        const std::size_t types = pops.types > 0 ? pops.types - 1 : 0;
        c->province_first.assign(markets + 1, 0);
        c->pop_first.assign(markets + 1, 0);
        for (std::size_t m = 1; m < markets; ++m) {
            c->province_first[m + 1] = c->province_first[m] + count[m];
            c->pop_first[m + 1] = c->pop_first[m] + static_cast<std::uint32_t>(count[m] * types);
        }
        if (markets > 0) {
            c->province_first[0] = c->province_first[1] = 0;
        }
        c->province.assign(markets > 0 ? c->province_first[markets] : 0, 0);
        c->pop_account.assign(markets > 0 ? c->pop_first[markets] : 0, 0);
        c->pop_type.assign(c->pop_account.size(), 0);
        std::vector<std::uint32_t> next(c->province_first.begin(), c->province_first.end());
        for (std::uint32_t p = 1; p < state.provinces.count; ++p) {
            const std::uint8_t m = market_of[p];
            if (m == 0) {
                continue;
            }
            const std::uint32_t k = next[m]++;
            c->province[k] = p;
            const std::size_t e = c->pop_first[m] + (k - c->province_first[m]) * types;
            for (std::size_t t = 1; t <= types; ++t) {
                c->pop_account[e + t - 1] = account(pops, t, p);
                c->pop_type[e + t - 1] = static_cast<std::uint8_t>(t);
            }
        }

        const std::size_t cells = state.prices.price.size();
        c->market_supply.assign(cells, 0.0f);
        c->market_demand.assign(cells, 0.0f);
    }

    void economy_day(const Content& content, WorldState* state, EconomyCache* cache)
    {
        mine(content, state, cache->money_good);
        produce(content, state, cache->money_good);
        measure_supply(content, *state, cache);
        for (std::size_t m = 1; m + 1 < cache->province_first.size(); ++m) {
            for (std::size_t tier = 0; tier < kNeedTiers; ++tier) {
                clear_tier(content, state, cache, m, tier);
            }
        }
        move_prices(content, state, *cache);
    }

    float needs_filled(const Content& content, const WorldState& state, std::uint32_t province, std::size_t tier)
    {
        const Pops& pops = state.pops;
        double want = 0.0;
        double got = 0.0;
        for (std::size_t t = 1; t < pops.types && t < content.pop_types.size(); ++t) {
            const double w = static_cast<double>(content.pop_types.need[t][tier]) * pops.at(t, province);
            want += w;
            got += w * pops.filled[tier][account(pops, t, province)];
        }
        return want > 0.0 ? static_cast<float>(got / want) : -1.0f;
    }
}
