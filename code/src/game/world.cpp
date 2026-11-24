#include "game/world.h"

#include "core/det_math.h"
#include "core/log.h"

namespace mc
{
    bool World::load(const std::filesystem::path& dir, const MapData& map, std::string* error)
    {
        if (!data_.load(dir, map, error)) {
            return false;
        }
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
        ++day_;
    }

    void World::restart()
    {
        day_ = kStartDay;
    }
}
