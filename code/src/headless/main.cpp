// mapcraft_sim.exe (docs/SIMULATION.md, "Headless-симулятор"): the simulation with no window, linked on mc_sim alone.
//
//     mapcraft_sim <scenario file> <run dir>
//
// Loads the scenario (headless/scenario.h), the map's simulation part, and data/ with the scenario's changes twice,
// into two worlds of their own; runs each the scenario's years a day at a time through World::advance_days, taking
// the state's checksum (CLAUDE.md, rule 11: here and only here) after every day; checks that both runs match day for
// day and that the money invariant held every day. The first run's record goes to <run dir> (headless/run_record.h).
// Exit codes: 0 all held, 1 bad arguments or a load or write failed, 2 the money invariant broke, 3 the runs differ.

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "core/log.h"
#include "core/map_data.h"
#include "game/calendar.h"
#include "game/world.h"
#include "headless/run_record.h"
#include "headless/scenario.h"
#include "headless/snapshot.h"

namespace
{
    constexpr int kOk = 0;
    constexpr int kFailed = 1;
    constexpr int kUnbalanced = 2;
    constexpr int kDiverged = 3;

    void to_stderr(mc::log::Level level, std::string_view message)
    {
        const char* tag = level == mc::log::Level::error ? "error: " : level == mc::log::Level::warn ? "warn: " : "";
        std::fprintf(stderr, "%s%.*s\n", tag, static_cast<int>(message.size()), message.data());
    }

    struct Run
    {
        std::unique_ptr<mc::World> world;
        std::vector<std::uint64_t> hashes;   // [0] the loaded state, then one per day
        double seconds = 0.0;                // in advance_days only
    };

    // One world from data/ with the scenario's changes, run its years; `record` (if any) gets the months. False if it
    // does not load.
    bool simulate(const mc::MapData& map, const mc::headless::Scenario& scenario, mc::headless::RunRecord* record,
                  Run* run)
    {
        run->world = std::make_unique<mc::World>();
        mc::World& world = *run->world;
        std::string error;
        if (!world.load(MC_DEFAULT_DATA, map, &error, scenario.patches)) {
            mc::log::error("{}", error);
            return false;
        }
        const std::uint32_t days = scenario.years * mc::kDaysPerYear;
        run->hashes.reserve(std::size_t{ days } + 1);
        run->hashes.push_back(mc::headless::checksum(world));
        if (record) {
            record->month(world);
        }
        for (std::uint32_t d = 0; d < days; ++d) {
            const auto begin = std::chrono::steady_clock::now();
            world.advance_days(1);
            run->seconds += std::chrono::duration<double>(std::chrono::steady_clock::now() - begin).count();
            run->hashes.push_back(mc::headless::checksum(world));
            if (record && world.day() % mc::kDaysPerMonth == 0) {
                record->month(world);
            }
        }
        return true;
    }
}

int main(int argc, char** argv)
{
    mc::log::set_sink(&to_stderr);
    if (argc != 3) {
        std::fprintf(stderr, "usage: mapcraft_sim <scenario file> <run dir>\n");
        return kFailed;
    }
    const std::filesystem::path run_dir = argv[2];

    mc::headless::Scenario scenario;
    std::string error;
    if (!mc::headless::load_scenario(argv[1], &scenario, &error)) {
        mc::log::error("{}", error);
        return kFailed;
    }
    mc::MapData map;
    if (!map.load(MC_DEFAULT_MAP, &error)) {
        mc::log::error("{}", error);
        return kFailed;
    }
    mc::log::info("scenario {}: {} years, seed {}, {} data changes", scenario.name, scenario.years, scenario.seed,
                  scenario.patches.size());

    mc::headless::RunRecord record;
    Run a;
    Run b;
    if (!simulate(map, scenario, &record, &a) || !simulate(map, scenario, nullptr, &b)) {
        return kFailed;
    }
    mc::log::info("{} years twice: {:.3f} s and {:.3f} s in the rules, {:.4f} s per year", scenario.years,
                  a.seconds, b.seconds, (a.seconds + b.seconds) / 2.0 / scenario.years);

    if (!record.write(run_dir, MC_ROOT, scenario, *a.world, a.hashes, &error)) {
        mc::log::error("{}", error);
        return kFailed;
    }
    mc::log::info("checksum {:016x}, the run in {}", a.hashes.back(), run_dir.string());

    if (!a.world->money_balanced() || !b.world->money_balanced()) {
        mc::log::error("the money invariant broke (the first day is logged above)");
        return kUnbalanced;
    }
    for (std::size_t d = 0; d < a.hashes.size(); ++d) {
        if (a.hashes[d] != b.hashes[d]) {
            mc::log::error("the two runs differ from day {} on: checksum {:016x} and {:016x}", mc::kStartDay + d,
                           a.hashes[d], b.hashes[d]);
            return kDiverged;
        }
    }
    mc::log::info("both runs match bit for bit every day; the money invariant held every day");
    return kOk;
}
