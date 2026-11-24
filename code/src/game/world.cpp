#include "game/world.h"

#include "core/det_math.h"
#include "core/log.h"
#include "game/economy/economy.h"
#include "game/economy/ledger.h"

namespace mc
{
    bool World::load(const std::filesystem::path& dir, const MapData& map, std::string* error)
    {
        if (!data_.load(dir, map, error)) {
            return false;
        }
        start_ = data_.state;
        prepare_economy(data_.content, data_.state, &economy_);
        log::info("world {}: {}", dir.string(), data_.summary());
        for (const std::string& w : data_.warnings) {
            log::warn("game data: {}", w);
        }
        return true;
    }

    void World::advance_days(std::uint32_t days)
    {
        det::set_sim_fp_env();
        for (std::uint32_t d = 0; d < days; ++d) {
            tick_day();
        }
    }

    void World::tick_day()
    {
        economy_day(data_.content, &data_.state, &economy_);
        ++day_;
        if (!unbalanced_ && !ledger::balanced(data_.state.money)) {
            unbalanced_ = true;   // reported once: after the first break every day would repeat it
            const Balances& money = data_.state.money;
            log::error("money invariant broken on day {}: total {} grains, start {} + minted {}", day_,
                       ledger::total(money), money.start, money.minted);
        }
    }

    void World::restart()
    {
        day_ = kStartDay;
        data_.state = start_;
        prepare_economy(data_.content, data_.state, &economy_);
        unbalanced_ = false;
    }
}
