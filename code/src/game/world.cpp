#include "game/world.h"

#include <chrono>

#include "core/log.h"

namespace mc
{
    bool World::load(const std::filesystem::path& dir, const MapData& map, std::string* error)
    {
        const auto started = std::chrono::steady_clock::now();
        if (!data_.load(dir, map, error)) {
            return false;
        }
        log::info("world {}: {}, {:.0f} ms", dir.string(), data_.summary(),
                  std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - started).count());
        for (const std::string& w : data_.warnings) {
            log::warn("game data: {}", w);
        }
        return true;
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
