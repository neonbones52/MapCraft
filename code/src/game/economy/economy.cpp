#include "game/economy/economy.h"

#include <algorithm>
#include <cmath>
#include <numeric>
#include <vector>

#include "game/economy/ledger.h"

namespace mc
{
    namespace
    {
        std::size_t account(const Pops& pops, std::size_t type, std::uint32_t province)
        {
            return type * pops.stride + province;
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
                    const std::size_t owners = account(pops, rgos.owners[rgo], p);
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

        // One tier of needs on one market (docs/GAMEDATA.md, "Экономика дня"): the market buys its pops' demand of
        // the tier from the goods that fill it, cheapest per unit of the tier first, at fixed prices; short supply
        // gives every pop the same share, min(1, S/D); a pop short of cash buys what its cash pays for. Each RGO
        // sells the same share of its stock of a good; the money goes to its workers and owners by its wage share.
        class TierClearing
        {
        public:
            TierClearing(const Content& content, WorldState* state, const std::vector<std::uint32_t>& provinces,
                         std::size_t tier)
                : content_(content), state_(*state), provinces_(provinces), tier_(tier),
                  goods_(content.needs.good[tier]), weights_(content.needs.weight[tier])
            {
            }

            void run()
            {
                const std::size_t n = goods_.size();
                order_.resize(n);
                std::iota(order_.begin(), order_.end(), std::size_t{ 0 });
                std::sort(order_.begin(), order_.end(), [&](std::size_t a, std::size_t b) {
                    const float ca = content_.goods_price[goods_[a]] / weights_[a];
                    const float cb = content_.goods_price[goods_[b]] / weights_[b];
                    return ca != cb ? ca < cb : goods_[a] < goods_[b];
                });
                supply_.assign(n, 0.0);
                for (std::size_t i = 0; i < n; ++i) {
                    for (const std::uint32_t p : provinces_) {
                        supply_[i] += state_.stock.at(goods_[i], p);
                    }
                }

                // What the pops want, then what the market has of it.
                const Pops& pops = state_.pops;
                double demand = 0.0;
                for (const std::uint32_t p : provinces_) {
                    for (std::size_t t = 1; t < pops.types; ++t) {
                        demand += static_cast<double>(content_.pop_types.need[t][tier_]) * pops.at(t, p);
                    }
                }
                if (demand <= 0.0) {
                    return;
                }
                const double filled = walk(demand);
                if (filled <= 0.0) {
                    return;   // nothing on the market fills this tier: every pop gets 0 (filled was reset)
                }
                const double share = filled / demand;               // min(1, S/D)
                const double cost = spent_ / filled;                 // emeralds per unit of the tier

                // What each pop can pay for.
                buyers_.clear();
                double bought = 0.0;
                for (const std::uint32_t p : provinces_) {
                    for (std::size_t t = 1; t < pops.types; ++t) {
                        const double want = static_cast<double>(content_.pop_types.need[t][tier_]) * pops.at(t, p);
                        if (want <= 0.0) {
                            continue;
                        }
                        const std::size_t a = account(pops, t, p);
                        const double cash = to_emeralds(state_.money.cash[a]);
                        const double fill = std::min(share * want, cash / cost);
                        buyers_.push_back({ a, want, fill });
                        bought += fill;
                    }
                }
                if (bought <= 0.0) {
                    return;
                }
                const double price = walk(bought) > 0.0 ? spent_ / bought : 0.0;   // cheapest first: <= cost

                payers_.clear();
                for (const Buyer& b : buyers_) {
                    const Money pay = std::min(to_grains(b.fill * price), state_.money.cash[b.account]);
                    payers_.push_back({ b.account, pay });
                    state_.pops.filled[tier_][b.account] = static_cast<float>(b.fill / b.want);
                }
                sell();
                ledger::settle(&state_.money, payers_, payees_, Flow::sales);
            }

        private:
            struct Buyer
            {
                std::size_t account;
                double want;    // units of the tier
                double fill;    // bought
            };

            // Buys `fill` units of the tier, cheapest first: taken_ per good, spent_ in emeralds; returns the fill got.
            double walk(double fill)
            {
                taken_.assign(goods_.size(), 0.0);
                spent_ = 0.0;
                double got = 0.0;
                for (const std::size_t i : order_) {
                    const double take = std::min(supply_[i] * weights_[i], fill - got);
                    if (take <= 0.0) {
                        continue;
                    }
                    taken_[i] = take / weights_[i];
                    spent_ += taken_[i] * content_.goods_price[goods_[i]];
                    got += take;
                }
                return got;
            }

            // Each RGO of the market sells the same share of its stock of each good taken; payees by revenue.
            void sell()
            {
                const RgoTypes& rgos = content_.rgo_types;
                payees_.clear();
                for (std::size_t i = 0; i < goods_.size(); ++i) {
                    if (taken_[i] <= 0.0 || supply_[i] <= 0.0) {
                        continue;
                    }
                    const double part = std::min(taken_[i] / supply_[i], 1.0);
                    for (const std::uint32_t p : provinces_) {
                        float& stock = state_.stock.at(goods_[i], p);
                        const std::uint8_t rgo = state_.provinces.rgo[p];
                        if (stock <= 0.0f || rgo == 0) {
                            continue;
                        }
                        const double sold = stock * part;
                        stock = part >= 1.0 ? 0.0f : static_cast<float>(stock - sold);
                        const double revenue = sold * content_.goods_price[goods_[i]];
                        const double wages = rgos.wages[rgo];
                        payees_.push_back({ account(state_.pops, rgos.workers[rgo], p), revenue * wages });
                        payees_.push_back({ account(state_.pops, rgos.owners[rgo], p), revenue * (1.0 - wages) });
                    }
                }
            }

            const Content& content_;
            WorldState& state_;
            const std::vector<std::uint32_t>& provinces_;
            std::size_t tier_;
            const std::vector<std::uint8_t>& goods_;
            const std::vector<float>& weights_;
            std::vector<std::size_t> order_;
            std::vector<double> supply_;
            std::vector<double> taken_;
            double spent_ = 0.0;
            std::vector<Buyer> buyers_;
            std::vector<ledger::Payment> payers_;
            std::vector<ledger::Share> payees_;
        };

        // Markets: the provinces of each market (by their region), then each tier in order on each market.
        void trade(const Content& content, WorldState* state)
        {
            for (Column<float>& tier : state->pops.filled) {
                std::fill(tier.begin(), tier.end(), 0.0f);
            }
            std::vector<std::vector<std::uint32_t>> markets(content.markets.size());
            for (std::uint32_t p = 1; p < state->provinces.count; ++p) {
                const std::uint16_t region = content.province_region[p];
                const std::uint8_t market = region < state->regions.count ? state->regions.market[region] : 0;
                if (market != 0) {
                    markets[market].push_back(p);
                }
            }
            for (std::size_t m = 1; m < markets.size(); ++m) {
                for (std::size_t tier = 0; tier < kNeedTiers; ++tier) {
                    TierClearing(content, state, markets[m], tier).run();
                }
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

    void economy_day(const Content& content, WorldState* state)
    {
        const int money_good = content.goods.find(kMoneyGood);
        mine(content, state, money_good);
        produce(content, state, money_good);
        trade(content, state);
    }
}
