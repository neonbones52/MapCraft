// mapcraft_sim.exe (docs/SIMULATION.md, "Headless-симулятор"): the simulation with no window, linked on mc_sim alone.
//
//     mapcraft_sim <scenario file> <run dir>
//
// Loads the scenario (headless/scenario.h), the map's simulation part, and data/ with the scenario's changes twice,
// into two worlds of their own; runs each the scenario's years a day at a time through World::advance_days, posting
// the scenario's commands of each day before it (World::post), taking the state's checksum (CLAUDE.md, rule 11: here
// and only here) after every day; checks that both runs match day for day, and their caches at the end. The first run's record goes to <run dir>
// (headless/run_record.h), the arrivals of its armies' marches and the changes of control and of owner also to
// stderr.
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
#include "game/economy/budget.h"
#include "game/economy/income.h"
#include "game/modifiers.h"
#include "game/occupation.h"
#include "game/war_score.h"
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
        // sieges.csv: each siege's beginning, phases and end (surrender, or lifted), with its numbers after it
        std::string sieges = "day,date,province,country,event,phases,roll,result,progress,breaches,garrison,"
                             "max_garrison,men,needed,impact,phase_days,surrender_chance\n";
        // control.csv: each change of a province's controller or owner (a peace)
        std::string control = "day,date,province,owner_before,owner,controller_before,controller\n";
        // wars.csv: each war's beginning, each change of its war score (game/war_score.h), its end (a peace) with
        // the score it ended at; from the attacker's side
        std::string wars = "day,date,war,attacker,defender,event,battles,attacker_occupation,defender_occupation,"
                           "complete,score\n";
        // treasury.csv: every country at the start and at each month's start (after the monthly tick): its
        // accounts in emeralds, its inflation, its monthly income (tax, production, gold) and census now
        // (game/economy/income.h), its sliders
        std::string treasury = "day,date,country,treasury,army,production,government,stability,inflation,"
                               "income_tax,income_production,income_gold,census,budget\n";
    };

    // Every country now, as rows of treasury.csv.
    void record_treasury(const mc::World& world, std::string* rows)
    {
        const mc::WorldState& state = world.state();
        const std::string date = mc::format(world.date());
        for (std::uint16_t c = 1; c < state.countries.count; ++c) {
            auto cash = [&](mc::Account a) { return mc::to_emeralds(state.money.cash[mc::account(c, a)]); };
            const mc::ProvinceIncome income = mc::country_income(state, world.content(), c);
            const mc::BudgetShares s = mc::budget_shares(state.countries, c);
            *rows += std::format("{},{},{},{:.6f},{:.6f},{:.6f},{:.6f},{:.6f},{:.4f},{:.6f},{:.6f},{:.6f},{:.6f},"
                                 "{}/{}/{}/{}/{}\n",
                                 world.day(), date, world.content().countries.tag_text(c),
                                 cash(mc::Account::treasury), cash(mc::Account::army), cash(mc::Account::production),
                                 cash(mc::Account::government), cash(mc::Account::stability),
                                 state.countries.inflation[c], income.tax, income.production, income.gold,
                                 income.census, s[0], s[1], s[2], s[3], s[4]);
        }
    }

    // The wars as wars.csv last saw them: per slot, whether it held a war, its generation, its countries and score.
    struct WarsSeen
    {
        std::vector<std::uint64_t> alive = std::vector<std::uint64_t>(mc::kWarSlots / 64, 0);
        std::vector<std::uint16_t> generation = std::vector<std::uint16_t>(mc::kWarSlots, 0);
        std::vector<std::uint16_t> attacker = std::vector<std::uint16_t>(mc::kWarSlots, 0);
        std::vector<std::uint16_t> defender = std::vector<std::uint16_t>(mc::kWarSlots, 0);
        std::vector<mc::WarScore> score = std::vector<mc::WarScore>(mc::kWarSlots);
    };

    // The wars' changes since `seen`, as rows of wars.csv; `seen` then holds today's.
    void record_wars(const mc::World& world, WarsSeen* seen, std::string* rows)
    {
        const mc::Wars& wars = world.state().wars;
        const mc::CountryDefinitions& countries = world.content().countries;
        const std::string date = mc::format(world.date());
        auto row = [&](std::uint32_t slot, const char* event, const mc::WarScore& s) {
            *rows += std::format("{},{},{},{},{},{},{},{},{},{},{}\n", world.day(), date, slot,
                                 countries.tag_text(seen->attacker[slot]), countries.tag_text(seen->defender[slot]),
                                 event, s.battles, s.attacker_occupation, s.defender_occupation, s.complete, s.total);
        };
        for (std::uint32_t slot = 0; slot < mc::kWarSlots; ++slot) {
            const bool was = (seen->alive[slot / 64] >> (slot % 64)) & 1u;
            const bool is = mc::slot_alive(wars.alive, slot);
            const bool same = was && is && wars.generation[slot] == seen->generation[slot];
            if (was && !same) {
                row(slot, "end", seen->score[slot]);
                seen->alive[slot / 64] &= ~(std::uint64_t{ 1 } << (slot % 64));
            }
            if (!is) {
                continue;
            }
            const mc::WarScore s = mc::war_score(world.state(), world.content(), static_cast<std::uint16_t>(slot));
            const mc::WarScore& last = seen->score[slot];
            const bool changed = s.battles != last.battles || s.attacker_occupation != last.attacker_occupation ||
                                 s.defender_occupation != last.defender_occupation || s.complete != last.complete ||
                                 s.total != last.total;
            seen->alive[slot / 64] |= std::uint64_t{ 1 } << (slot % 64);
            seen->generation[slot] = wars.generation[slot];
            seen->attacker[slot] = wars.attacker[slot];
            seen->defender[slot] = wars.defender[slot];
            seen->score[slot] = s;
            if (!same) {
                row(slot, "begin", s);
            } else if (changed) {
                row(slot, "score", s);
            }
        }
    }

    // What the sieges' slots held before a day, and the provinces' controllers.
    struct SiegesBefore
    {
        std::vector<std::uint64_t> alive;
        std::vector<std::uint16_t> generation;
        std::vector<std::uint16_t> phases;
        std::vector<std::uint16_t> province;
        std::vector<std::uint16_t> country;
        std::vector<std::uint16_t> controller;
        std::vector<std::uint16_t> owner;

        void take(const mc::WorldState& state)
        {
            const mc::Sieges& s = state.sieges;
            alive.assign(s.alive.begin(), s.alive.end());
            generation.assign(s.generation.begin(), s.generation.end());
            phases.assign(s.phases.begin(), s.phases.end());
            province.assign(s.province.begin(), s.province.end());
            country.assign(s.country.begin(), s.country.end());
            controller.assign(state.provinces.controller.begin(), state.provinces.controller.end());
            owner.assign(state.provinces.owner.begin(), state.provinces.owner.end());
        }
    };

    constexpr std::string_view kSiegeResults[] = { "", "status_quo", "disease", "breach", "supplies", "food", "water",
                                                   "desertion" };

    // The sieges' and the controllers' and owners' changes over the day just passed, as rows of sieges.csv and
    // control.csv; with `log`, each change of a controller or an owner also to stderr.
    void record_sieges(const mc::MapData& map, const mc::World& world, const SiegesBefore& before, bool log,
                       std::string* sieges, std::string* control)
    {
        const mc::WorldState& state = world.state();
        const mc::Sieges& s = state.sieges;
        const mc::Provinces& provinces = state.provinces;
        const mc::CountryDefinitions& countries = world.content().countries;
        const std::string date = mc::format(world.date());
        for (std::uint32_t slot = 0; slot < mc::kSiegeSlots; ++slot) {
            const bool was = (before.alive[slot / 64] >> (slot % 64)) & 1u;
            const bool same = was && mc::slot_alive(s.alive, slot) && s.generation[slot] == before.generation[slot];
            if (was && !same) {
                const std::uint16_t p = before.province[slot];
                const bool taken = provinces.controller[p] == before.country[slot] &&
                                   before.controller[p] != before.country[slot];
                *sieges += std::format("{},{},{},{},{},,,,{},,{},,,,,,\n", world.day(), date, p,
                                       countries.tag_text(before.country[slot]), taken ? "surrender" : "lifted",
                                       provinces.siege_progress[p], provinces.garrison[p]);
            }
            if (!mc::slot_alive(s.alive, slot) || (same && s.phases[slot] == before.phases[slot])) {
                continue;
            }
            const auto siege = static_cast<std::uint16_t>(slot);
            const std::uint16_t p = s.province[slot];
            const mc::Besiegers b = mc::besiegers(state, world.content(), siege);
            const mc::SiegeNumbers n =
                mc::siege_numbers(state, world.content(), world.cache(), mc::has_port(map, p), siege, b);
            const mc::SiegeChances chances =
                mc::siege_chances(world.content().combat, n, s.breaches[slot], provinces.garrison[p]);
            *sieges += std::format("{},{},{},{},{},{},{},{},{},{},{},{},{},{},{},{},{}/{}\n", world.day(), date, p,
                                   countries.tag_text(s.country[slot]), same ? "phase" : "begin", s.phases[slot],
                                   s.roll[slot], kSiegeResults[static_cast<std::size_t>(s.result[slot])],
                                   provinces.siege_progress[p], s.breaches[slot], provinces.garrison[p],
                                   n.max_garrison, b.men, n.needed, n.impact, n.phase_days, chances.surrender,
                                   chances.sides);
        }
        for (std::uint32_t p = 1; p < provinces.count; ++p) {
            const bool owner = provinces.owner[p] != before.owner[p];
            const bool controller = provinces.controller[p] != before.controller[p];
            if (!owner && !controller) {
                continue;
            }
            *control += std::format("{},{},{},{},{},{},{}\n", world.day(), date, p,
                                    countries.tag_text(before.owner[p]), countries.tag_text(provinces.owner[p]),
                                    countries.tag_text(before.controller[p]),
                                    countries.tag_text(provinces.controller[p]));
            if (log && owner) {
                mc::log::info("{} (day {} of the run): {} ({}) is owned by {}", date, world.day() - mc::kStartDay,
                              map.province_name(p), p, countries.tag_text(provinces.owner[p]));
            }
            if (log && controller) {
                mc::log::info("{} (day {} of the run): {} ({}) is controlled by {}", date, world.day() - mc::kStartDay,
                              map.province_name(p), p, countries.tag_text(provinces.controller[p]));
            }
        }
    }

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
            commands.back().provinces = c.provinces;   // the scenario outlives the run
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
        record_treasury(world, &run->treasury);
        std::vector<std::uint16_t> before(mc::kArmySlots);
        SiegesBefore sieges_before;
        WarsSeen wars_seen;
        std::size_t next = 0;   // the scenario's next command
        for (std::uint32_t d = 0; d < days; ++d) {
            for (; next < commands.size() && scenario.commands[next].day == d; ++next) {
                world.post(commands[next]);
            }
            const mc::Armies& armies = world.state().armies;
            std::copy(armies.province.begin(), armies.province.end(), before.begin());
            sieges_before.take(world.state());
            const auto begin = std::chrono::steady_clock::now();
            world.advance_days(1);
            run->seconds += std::chrono::duration<double>(std::chrono::steady_clock::now() - begin).count();
            run->hashes.push_back(mc::headless::checksum(world));
            record_moves(map, world, before, log, &run->moves);
            record_sieges(map, world, sieges_before, log, &run->sieges, &run->control);
            record_wars(world, &wars_seen, &run->wars);
            if (world.day() % mc::kDaysPerMonth == 0) {
                record_armies(world, &run->armies);
                record_treasury(world, &run->treasury);
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

    const mc::headless::RunSeries series[] = {
        { "moves.csv", a.moves }, { "armies.csv", a.armies }, { "sieges.csv", a.sieges }, { "control.csv", a.control },
        { "wars.csv", a.wars }, { "treasury.csv", a.treasury }
    };
    if (!mc::headless::write_run(run_dir, MC_ROOT, scenario, *a.world, a.hashes, series, &error)) {
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
