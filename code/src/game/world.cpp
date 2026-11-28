#include "game/world.h"

#include <format>
#include <string>

#include "core/det_math.h"
#include "core/log.h"
#include "game/peace.h"
#include "game/war.h"

namespace mc
{
    bool World::load(const std::filesystem::path& dir, const MapData& map, std::uint32_t seed, std::string* error,
                     std::span<const csv::Patch> patches)
    {
        if (!data_.load(dir, map, error, patches)) {
            return false;
        }
        start_ = data_.state;
        day_ = kStartDay;
        seed_ = seed;
        movement_.build(map, data_.content, data_.state);
        movement_.rebuild_steps(data_.state);
        combat_.build(map);
        occupation_.build(map, data_.content);
        recovery_.build();
        commands_ = {};
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
            run_commands();
            tick_day();
        }
    }

    void World::run_commands()
    {
        if (commands_.empty()) {
            return;
        }
        det::set_sim_fp_env();
        commands_.run_due(day_, [this](const Command& command) { apply(command); });
    }

    void World::apply(const Command& command)
    {
        WorldState& state = data_.state;
        switch (command.kind) {
        case CommandKind::move_army: {
            auto reject = [&](const char* why) {
                const CountryDefinitions& countries = data_.content.countries;
                log::warn("day {}: {} orders army {}:{} to province {}: {}", day_,
                          command.country < countries.size() ? countries.tag_text(command.country) : "?",
                          command.army.slot, command.army.generation, command.province, why);
            };
            std::uint16_t slot = 0;
            if (!resolve(state.armies, command.army, &slot)) {
                return reject("no such army (dead, or never was)");
            }
            if (command.country == 0 || state.armies.owner[slot] != command.country) {
                return reject("not the country's army");
            }
            if (state.armies.battle[slot] != 0) {
                return reject("in a battle");
            }
            if ((state.armies.retreating[slot / 64] >> (slot % 64)) & 1u) {
                return reject("retreating");
            }
            if (!movement_.order(state, slot, command.province)) {
                return reject("no way there over land");
            }
            return;
        }
        case CommandKind::declare_war: {
            const CountryDefinitions& countries = data_.content.countries;
            auto tag = [&](std::uint16_t c) { return c < countries.size() ? countries.tag_text(c) : std::string("?"); };
            if (const char* why = why_no_war(state, data_.cache.at_war, command.country, command.target, day_)) {
                log::warn("day {}: {} declares war on {}: {}", day_, tag(command.country), tag(command.target), why);
                return;
            }
            declare_war(state, data_.cache.at_war, command.country, command.target, day_);
            log::info("day {}: {} declares war on {}", day_, tag(command.country), tag(command.target));
            return;
        }
        case CommandKind::peace: {
            const CountryDefinitions& countries = data_.content.countries;
            auto tag = [&](std::uint16_t c) { return c < countries.size() ? countries.tag_text(c) : std::string("?"); };
            const std::string kind = command.provinces.empty()
                                         ? std::string("a white peace")
                                         : std::format("a peace of {} provinces", command.provinces.size());
            if (const char* why = why_no_peace(state, data_.cache.at_war, command.country, command.target,
                                               command.provinces)) {
                log::warn("day {}: {} offers {} {}: {}", day_, tag(command.country), tag(command.target), kind, why);
                return;
            }
            const PeaceAnswer answer =
                peace_answer(state, data_.content, command.country, command.target, command.provinces);
            log::info("day {}: {} offers {} {}, cost {:.3f}, war score {}: {}", day_, tag(command.country),
                      tag(command.target), kind, fx_to_double(answer.cost), answer.score,
                      answer.accepted ? "accepted" : "refused");
            if (answer.accepted) {
                make_peace(state, data_.content, data_.cache.at_war, command.country, command.target,
                           command.provinces, day_);
            }
            return;
        }
        case CommandKind::budget: {
            const CountryDefinitions& countries = data_.content.countries;
            const std::string tag = command.country < countries.size() ? countries.tag_text(command.country) : "?";
            const BudgetShares& s = command.shares;
            if (const char* why = why_no_budget(state, command.country, s)) {
                log::warn("day {}: {} sets its budget to {}/{}/{}/{}/{}: {}", day_, tag, s[0], s[1], s[2], s[3], s[4],
                          why);
                return;
            }
            set_budget(state, command.country, s);
            log::info("day {}: {} sets its budget to {}/{}/{}/{}/{}", day_, tag, s[0], s[1], s[2], s[3], s[4]);
            return;
        }
        case CommandKind::none:
            break;
        }
        log::warn("day {}: a command of an unknown kind {}", day_, static_cast<unsigned>(command.kind));
    }

    void World::tick_day()
    {
        movement_.day(data_.state);
        combat_.day(data_.state, data_.content, data_.cache, movement_, seed_, day_);
        occupation_.day(data_.state, data_.content, data_.cache, seed_, day_);
        ++day_;
        if (day_ % kDaysPerMonth == 0) {
            tick_month();
        }
    }

    void World::tick_month()
    {
        if (date().month == 1) {
            tick_year();
        }
        recovery_.month(data_.state, data_.content, data_.cache);
        occupation_.month(data_.state, data_.content);
        expire_truces(data_.state.truces, day_);
        budget_.month(data_.state, data_.content);
    }

    void World::tick_year()
    {
        budget_.census(data_.state, data_.content);
    }

    void World::restart(std::uint32_t seed)
    {
        day_ = kStartDay;
        seed_ = seed;
        data_.state = start_;
        data_.rebuild_cache();
        movement_.rebuild_steps(data_.state);
        commands_ = {};
    }
}
