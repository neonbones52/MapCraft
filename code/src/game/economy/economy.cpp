#include "game/economy/economy.h"

#include <algorithm>
#include <cmath>
#include <numeric>

#include "game/economy/ledger.h"

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

        // Buys `fill` units of a tier from its goods in `order`, cheapest first, out of `supply`: what each good gives
        // goes to `taken`; returns the fill got and the emeralds it costs in `spent`.
        double walk(const Content& content, std::size_t tier, const std::vector<std::uint8_t>& order,
                    const std::vector<double>& supply, double fill, std::vector<double>* taken, double* spent)
        {
            const std::vector<std::uint8_t>& goods = content.needs.good[tier];
            const std::vector<float>& weights = content.needs.weight[tier];
            std::fill(taken->begin(), taken->end(), 0.0);
            *spent = 0.0;
            double got = 0.0;
            for (const std::uint8_t i : order) {
                const double take = std::min(supply[i] * weights[i], fill - got);
                if (take <= 0.0) {
                    continue;
                }
                (*taken)[i] = take / weights[i];
                *spent += (*taken)[i] * content.goods_price[goods[i]];
                got += take;
            }
            return got;
        }

        // One tier of needs on one market (docs/GAMEDATA.md, "Экономика дня"): the market buys its pops' demand of
        // the tier from the goods that fill it, cheapest per unit of the tier first, at fixed prices; short supply
        // gives every pop the same share, min(1, S/D); a pop short of cash buys what its cash pays for. Each RGO
        // sells the same share of its stock of a good; the money goes to its workers and owners by its wage share.
        // Every step is a pass over the market's columns in the cache.
        void clear_tier(const Content& content, WorldState* state, EconomyCache* c, std::size_t market,
                        std::size_t tier)
        {
            const std::vector<std::uint8_t>& goods = content.needs.good[tier];
            const std::vector<std::uint8_t>& order = c->tier_order[tier];
            const std::uint32_t p0 = c->province_first[market];
            const std::uint32_t p1 = c->province_first[market + 1];
            const std::uint32_t e0 = c->pop_first[market];
            const std::uint32_t e1 = c->pop_first[market + 1];
            const std::size_t pops = e1 - e0;
            Column<float>& filled = state->pops.filled[tier];

            // Demand, per pop and in all.
            c->want.resize(pops);
            double demand = 0.0;
            for (std::size_t e = 0; e < pops; ++e) {
                c->want[e] = static_cast<double>(content.pop_types.need[c->pop_type[e0 + e]][tier]) *
                             state->pops.size[c->pop_account[e0 + e]];
                demand += c->want[e];
            }
            // Supply: the stock of each good of the tier over the market's provinces.
            c->supply.assign(goods.size(), 0.0);
            c->taken.assign(goods.size(), 0.0);
            for (std::size_t i = 0; i < goods.size(); ++i) {
                const float* row = &state->stock.amount[goods[i] * state->stock.stride];
                for (std::uint32_t k = p0; k < p1; ++k) {
                    c->supply[i] += row[c->province[k]];
                }
            }
            double spent = 0.0;
            const double got = demand > 0.0 ? walk(content, tier, order, c->supply, demand, &c->taken, &spent) : 0.0;
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
            const double paid = bought > 0.0 ? walk(content, tier, order, c->supply, bought, &c->taken, &spent) : 0.0;
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
            for (std::size_t i = 0; i < goods.size(); ++i) {
                if (c->taken[i] <= 0.0 || c->supply[i] <= 0.0) {
                    continue;
                }
                const double part = std::min(c->taken[i] / c->supply[i], 1.0);
                const double unit = content.goods_price[goods[i]];
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
                    c->weight.push_back(sold * unit * wages);
                    c->payee.push_back(account(state->pops, rgos.owners[rgo], p));
                    c->weight.push_back(sold * unit * (1.0 - wages));
                }
            }
            ledger::settle(&state->money, { c->pop_account.data() + e0, pops }, c->pay, c->payee, c->weight, Flow::sales);
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

        // The goods of each tier by price per unit of the tier (prices are fixed), then by good: a full order.
        for (std::size_t tier = 0; tier < kNeedTiers; ++tier) {
            const std::vector<std::uint8_t>& goods = content.needs.good[tier];
            const std::vector<float>& weights = content.needs.weight[tier];
            std::vector<std::uint8_t>& order = c->tier_order[tier];
            order.resize(goods.size());
            std::iota(order.begin(), order.end(), std::uint8_t{ 0 });
            std::sort(order.begin(), order.end(), [&](std::uint8_t a, std::uint8_t b) {
                const float ca = content.goods_price[goods[a]] / weights[a];
                const float cb = content.goods_price[goods[b]] / weights[b];
                return ca != cb ? ca < cb : goods[a] < goods[b];
            });
        }
    }

    void economy_day(const Content& content, WorldState* state, EconomyCache* cache)
    {
        mine(content, state, cache->money_good);
        produce(content, state, cache->money_good);
        for (std::size_t m = 1; m + 1 < cache->province_first.size(); ++m) {
            for (std::size_t tier = 0; tier < kNeedTiers; ++tier) {
                clear_tier(content, state, cache, m, tier);
            }
        }
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
