// mapcraft_sim.exe (docs/SIMULATION.md, "Headless-симулятор"): the simulation with no window, linked on mc_sim alone.
//
//     mapcraft_sim <scenario file> <run dir>
//
// Loads the scenario (headless/scenario.h), the map's simulation part, and data/ with the scenario's changes twice,
// into two worlds of their own; runs each the scenario's years a day at a time through World::advance_days, posting
// the scenario's commands of each day before it (World::post), taking the state's checksum (CLAUDE.md, rule 11: here
// and only here) after every day; checks that both runs match day for day, and their caches at the end. The first run's record goes to <run dir>
// (headless/run_record.h), the arrivals of its armies' marches also to stderr.
// Exit codes: 0 the runs match, 1 bad arguments or a load or write failed, 3 the runs differ.

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <algorithm>
#include <filesystem>
#include <format>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "core/log.h"
#include "core/map_data.h"
#include "game/armies.h"
#include "game/calendar.h"
#include "game/modifiers.h"
#include "game/world.h"
#include "headless/run_record.h"
#include "headless/scenario.h"
#include "headless/snapshot.h"

namespace
{
    constexpr int kOk = 0;
    constexpr int kFailed = 1;
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
        std::string moves = "day,date,army,generation,province,next\n";   // moves.csv: each step an army ends
        // armies.csv: every live army at the start and at each month's start (after the monthly tick)
        std::string armies = "day,date,army,generation,owner,province,battle,retreating,regiments,men,morale,"
                             "max_morale\n";
    };

    // Every live army now, as rows of armies.csv: its place, whether it fights or retreats, its regiments, men and
    // their average morale, and its country's maximum morale.
    void record_armies(const mc::World& world, std::string* rows)
    {
        const mc::Armies& armies = world.state().armies;
        const std::string date = mc::format(world.date());
        mc::for_each_alive(armies.alive, [&](std::uint32_t a) {
            const auto slot = static_cast<std::uint16_t>(a);
            const mc::ArmyTotals totals = mc::army_totals(world.state().regiments, slot);
            const std::uint16_t owner = armies.owner[a];
            *rows += std::format("{},{},{},{},{},{},{},{},{},{},{:.3f},{:.3f}\n", world.day(), date, a,
                                 armies.generation[a], world.content().countries.tag_text(owner), armies.province[a],
                                 armies.battle[a] != 0 ? 1 : 0, (armies.retreating[a / 64] >> (a % 64)) & 1u,
                                 totals.regiments, totals.men, mc::fx_to_double(totals.morale),
                                 mc::fx_to_double(mc::max_morale(world.cache().country_modifiers, owner)));
        });
    }

    // The armies whose province changed over the day just passed, as rows of moves.csv (`before`: their provinces
    // before it, per slot); with `log`, each arrival where a march ends also to stderr.
    void record_moves(const mc::MapData& map, const mc::World& world, const std::vector<std::uint16_t>& before,
                      bool log, std::string* rows)
    {
        const mc::Armies& armies = world.state().armies;
        const std::string date = mc::format(world.date());
        mc::for_each_alive(armies.alive, [&](std::uint32_t a) {
            const std::uint16_t province = armies.province[a];
            if (province == before[a]) {
                return;
            }
            *rows += std::format("{},{},{},{},{},{}\n", world.day(), date, a, armies.generation[a], province,
                                 armies.next[a]);
            if (log && armies.next[a] == 0) {
                mc::log::info("{} (day {} of the run): army {}:{} arrives in {} ({})", date,
                              world.day() - mc::kStartDay, a, armies.generation[a], map.province_name(province),
                              province);
            }
        });
    }

    // One world from data/ with the scenario's changes, run its years. False if it does not load or a command names a
    // country it does not know. `log`: the arrivals to stderr (the first run).
    bool simulate(const mc::MapData& map, const mc::headless::Scenario& scenario, bool log, Run* run)
    {
        run->world = std::make_unique<mc::World>();
        mc::World& world = *run->world;
        std::string error;
        if (!world.load(MC_DEFAULT_DATA, map, static_cast<std::uint32_t>(scenario.seed), &error, scenario.patches)) {
            mc::log::error("{}", error);
            return false;
        }
        std::vector<mc::Command> commands;
        for (const mc::headless::ScenarioCommand& c : scenario.commands) {
            const int country = world.content().countries.find(c.country);
            if (country <= 0) {
                mc::log::error("{}: unknown country \"{}\"", c.origin, c.country);
                return false;
            }
            commands.push_back(c.command);
            commands.back().country = static_cast<std::uint16_t>(country);
            if (!c.target.empty()) {
                const int target = world.content().countries.find(c.target);
                if (target <= 0) {
                    mc::log::error("{}: unknown country \"{}\"", c.origin, c.target);
                    return false;
                }
                commands.back().target = static_cast<std::uint16_t>(target);
            }
        }
        const std::uint32_t days = scenario.years * mc::kDaysPerYear;
        run->hashes.reserve(std::size_t{ days } + 1);
        run->hashes.push_back(mc::headless::checksum(world));
        record_armies(world, &run->armies);
        std::vector<std::uint16_t> before(mc::kArmySlots);
        std::size_t next = 0;   // the scenario's next command
        for (std::uint32_t d = 0; d < days; ++d) {
            for (; next < commands.size() && scenario.commands[next].day == d; ++next) {
                world.post(commands[next]);
            }
            const mc::Armies& armies = world.state().armies;
            std::copy(armies.province.begin(), armies.province.end(), before.begin());
            const auto begin = std::chrono::steady_clock::now();
            world.advance_days(1);
            run->seconds += std::chrono::duration<double>(std::chrono::steady_clock::now() - begin).count();
            run->hashes.push_back(mc::headless::checksum(world));
            record_moves(map, world, before, log, &run->moves);
            if (world.day() % mc::kDaysPerMonth == 0) {
                record_armies(world, &run->armies);
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
    mc::log::info("scenario {}: {} years, seed {}, {} data changes, {} commands", scenario.name, scenario.years,
                  scenario.seed, scenario.patches.size(), scenario.commands.size());

    Run a;
    Run b;
    if (!simulate(map, scenario, true, &a) || !simulate(map, scenario, false, &b)) {
        return kFailed;
    }
    mc::log::info("{} years twice: {:.3f} s and {:.3f} s in the rules, {:.4f} s per year", scenario.years,
                  a.seconds, b.seconds, (a.seconds + b.seconds) / 2.0 / scenario.years);

    if (!mc::headless::write_run(run_dir, MC_ROOT, scenario, *a.world, a.hashes, a.moves, a.armies, &error)) {
        mc::log::error("{}", error);
        return kFailed;
    }
    mc::log::info("checksum {:016x}, the run in {}", a.hashes.back(), run_dir.string());

    for (std::size_t d = 0; d < a.hashes.size(); ++d) {
        if (a.hashes[d] != b.hashes[d]) {
            mc::log::error("the two runs differ from day {} on: checksum {:016x} and {:016x}", mc::kStartDay + d,
                           a.hashes[d], b.hashes[d]);
            return kDiverged;
        }
    }
    // The cache is not in the checksum (never saved): the runs' caches at the end, compared whole.
    if (mc::headless::dump_cache(*a.world) != mc::headless::dump_cache(*b.world)) {
        mc::log::error("the two runs' caches differ at the end (cache.csv)");
        return kDiverged;
    }
    mc::log::info("both runs match bit for bit every day, and their caches at the end");
    return kOk;
}
