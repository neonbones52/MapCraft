#include "game/economy/economy.h"

#include "game/economy/ledger.h"

namespace mc
{
    namespace
    {
        // Mining: an RGO whose outputs hold the money good mints, every day, workers x its amount per worker into
        // the province's pop of its owners' type (rgo_types.csv). Its other goods wait for production (M3).
        void mine(const Content& content, WorldState* state)
        {
            const RgoTypes& rgos = content.rgo_types;
            const int money_good = content.goods.find(kMoneyGood);
            if (money_good <= 0) {
                return;
            }
            Pops& pops = state->pops;
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
                    ledger::mint(&state->money, rgos.owners[rgo] * pops.stride + p, amount, Flow::mine);
                }
            }
        }
    }

    void economy_day(const Content& content, WorldState* state)
    {
        mine(content, state);
    }
}
