#include "game/peace.h"

#include <algorithm>

#include "game/armies.h"
#include "game/calendar.h"
#include "game/combat.h"
#include "game/countries.h"
#include "game/occupation.h"
#include "game/war.h"
#include "game/war_score.h"

namespace mc
{
    namespace
    {
        // The province changes hands: its controller is `controller`, its siege's progress gone.
        void set_controller(Provinces& provinces, std::uint16_t province, std::uint16_t controller)
        {
            if (provinces.controller[province] != controller) {
                provinces.controller[province] = controller;
                provinces.siege_progress[province] = 0;
            }
        }
    }

    const char* why_no_peace(const WorldState& state, const AtWar& at_war, std::uint16_t from, std::uint16_t to,
                             std::span<const std::uint16_t> provinces)
    {
        const std::size_t countries = state.countries.count;
        if (from == 0 || to == 0 || from >= countries || to >= countries || !at_war.has(from, to)) {
            return "not at war";
        }
        const Provinces& p = state.provinces;
        for (std::size_t i = 0; i < provinces.size(); ++i) {
            const std::uint16_t province = provinces[i];
            if (province == 0 || province >= p.count || p.owner[province] != to) {
                return "a province not the enemy's";
            }
            if (std::find(provinces.begin(), provinces.begin() + static_cast<std::ptrdiff_t>(i), province) !=
                provinces.begin() + static_cast<std::ptrdiff_t>(i)) {
                return "a province twice";
            }
        }
        if (!provinces.empty()) {
            std::size_t owned = 0;
            for (std::uint32_t id = 1; id < p.count; ++id) {
                owned += p.owner[id] == to ? 1 : 0;
            }
            if (owned == provinces.size()) {
                return "every province of the enemy";
            }
        }
        return nullptr;
    }

    Fixed peace_cost(const WorldState& state, const Content& content, std::uint16_t to,
                     std::span<const std::uint16_t> provinces)
    {
        if (provinces.empty()) {
            return 0;
        }
        const CountryWarFacts facts = country_war_facts(state, content, to);
        Fixed cost = 0;
        for (const std::uint16_t province : provinces) {
            cost += province_value(state, content, province, facts);
        }
        return cost;
    }

    PeaceAnswer peace_answer(const WorldState& state, const Content& content, std::uint16_t from, std::uint16_t to,
                             std::span<const std::uint16_t> provinces)
    {
        PeaceAnswer answer;
        answer.score = war_score_of(state, content, from, to);
        answer.cost = peace_cost(state, content, to, provinces);
        answer.accepted = provinces.empty() ? answer.score > 0
                                            : std::int64_t{ answer.cost } <= std::int64_t{ answer.score } * kFixedOne;
        return answer;
    }

    void make_peace(WorldState& state, const Content& content, AtWar& at_war, std::uint16_t from, std::uint16_t to,
                    std::span<const std::uint16_t> provinces, std::uint32_t day)
    {
        const int war = war_between(state.wars, from, to);
        if (war >= 0) {
            end_war(state, at_war, static_cast<std::uint16_t>(war));
        }
        for_each_alive(state.battles.alive, [&](std::uint32_t b) {
            const Battle& battle = state.battles.list[b];
            if (!at_war.has(battle.sides[0].country, battle.sides[1].country)) {
                end_battle(state, static_cast<std::uint16_t>(b));
            }
        });

        Provinces& p = state.provinces;
        for (std::uint32_t id = 1; id < p.count; ++id) {
            const auto province = static_cast<std::uint16_t>(id);
            if ((p.owner[id] == from && p.controller[id] == to) || (p.owner[id] == to && p.controller[id] == from)) {
                set_controller(p, province, p.owner[id]);
            }
        }
        const std::uint16_t capital = state.countries.capital[to];
        bool capital_lost = false;
        for (const std::uint16_t province : provinces) {
            capital_lost = capital_lost || province == capital;
            p.owner[province] = from;
            set_controller(p, province, from);
        }
        if (capital_lost) {
            std::uint16_t best = 0;
            for (std::uint32_t id = 1; id < p.count; ++id) {
                if (p.owner[id] == to && (best == 0 || p.base_tax[id] > p.base_tax[best])) {
                    best = static_cast<std::uint16_t>(id);
                }
            }
            state.countries.capital[to] = best;
        }
        for (std::uint32_t id = 1; id < p.count; ++id) {
            if (p.owner[id] == from || p.owner[id] == to) {
                const auto province = static_cast<std::uint16_t>(id);
                p.garrison[id] = std::min(p.garrison[id], max_garrison(content, fort_level(state, content, province)));
            }
        }
        for_each_alive(state.sieges.alive, [&](std::uint32_t s) {
            if (!may_besiege(state, at_war, state.sieges.country[s], state.sieges.province[s])) {
                lift_siege(state.sieges, static_cast<std::uint16_t>(s));
            }
        });
        set_truce(state.truces, from, to,
                  day + static_cast<std::uint32_t>(content.war.truce_years) * kDaysPerYear);
    }
}
