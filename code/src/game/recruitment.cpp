#include "game/recruitment.h"

#include <limits>

#include "core/log.h"
#include "game/armies.h"
#include "game/economy/ledger.h"
#include "game/economy/upkeep.h"
#include "game/modifiers.h"

namespace mc
{
    namespace
    {
        bool besieged(const Sieges& sieges, std::uint16_t province)
        {
            bool found = false;
            for_each_alive(sieges.alive, [&](std::uint32_t s) { found = found || sieges.province[s] == province; });
            return found;
        }

        // The regiments queued in `province` (of any country).
        std::uint32_t queued(const Recruits& recruits, std::uint16_t province)
        {
            std::uint32_t n = 0;
            for_each_alive(recruits.alive, [&](std::uint32_t r) { n += recruits.province[r] == province ? 1u : 0u; });
            return n;
        }

        // The recruit in `slot` leaves its queue: the ones behind it move up a place.
        void drop(Recruits& recruits, std::uint16_t slot)
        {
            const std::uint16_t province = recruits.province[slot];
            const std::uint8_t place = recruits.place[slot];
#define MC_CLEAR_SLOT(table, type, name, start) recruits.name[slot] = type{};
            MC_RECRUIT_COLUMNS(MC_CLEAR_SLOT)
#undef MC_CLEAR_SLOT
            free_slot(recruits.alive, slot);
            for_each_alive(recruits.alive, [&](std::uint32_t r) {
                if (recruits.province[r] == province && recruits.place[r] > place) {
                    --recruits.place[r];
                }
            });
        }

        // A new army of one full regiment of the recruit's in its province, at recruit_morale; false (nothing made) with no slot.
        bool stand(WorldState& state, const Content& content, const Cache& cache, std::uint16_t slot,
                   std::uint16_t* army)
        {
            const Recruits& recruits = state.recruits;
            const std::uint16_t country = recruits.country[slot];
            const Fixed full = fx_mul(max_morale(cache.country_modifiers, country), content.economy.recruit_morale);
            const Fixed ceiling = morale_ceiling(cache.country_modifiers, state.countries, content.economy, country);
            if (!create_army(state.armies, country, recruits.province[slot], army)) {
                return false;
            }
            std::uint16_t regiment = 0;
            if (!add_regiment(state.regiments, *army, recruits.type[slot], kFullRegiment,
                              full < ceiling ? full : ceiling, &regiment)) {
                destroy_army(state, *army);
                return false;
            }
            return true;
        }
    }

    // A siege holds the queue unless its besieger fights the recruiter in a battle in the province.
    bool recruitment_paused(const WorldState& state, std::uint16_t province, std::uint16_t country)
    {
        const Sieges& sieges = state.sieges;
        const Battles& battles = state.battles;
        bool held = false;
        for_each_alive(sieges.alive, [&](std::uint32_t s) {
            if (sieges.province[s] != province) {
                return;
            }
            bool fought = false;
            for_each_alive(battles.alive, [&](std::uint32_t b) {
                const Battle& battle = battles.list[b];
                const std::uint16_t a = battle.sides[0].country;
                const std::uint16_t d = battle.sides[1].country;
                fought = fought || (battle.province == province &&
                                    ((a == sieges.country[s] && d == country) ||
                                     (d == sieges.country[s] && a == country)));
            });
            held = held || !fought;
        });
        return held;
    }

    int recruit_time(const Content& content, std::uint8_t type)
    {
        const EconomyDefines& e = content.economy;
        switch (content.regiment_category[type]) {
        case RegimentCategory::infantry:
            return e.infantry_time;
        case RegimentCategory::cavalry:
            return e.cavalry_time;
        case RegimentCategory::artillery:
            return e.artillery_time;
        }
        return e.infantry_time;
    }

    Money recruit_cost(const WorldState& state, const Content& content, std::uint16_t country, std::uint8_t type)
    {
        return to_grains(static_cast<double>(content.regiment_cost[type]) *
                         (1.0 + static_cast<double>(state.countries.inflation[country]) / 100.0));
    }

    const char* why_no_recruit(const WorldState& state, const Content& content, std::uint16_t country,
                               std::uint16_t province, std::uint8_t type)
    {
        if (country == 0 || country >= state.countries.count) {
            return "no such country";
        }
        if (type == 0 || type >= content.regiment_types.size()) {
            return "no such regiment type";
        }
        const Provinces& provinces = state.provinces;
        if (province == 0 || province >= provinces.count || provinces.owner[province] != country) {
            return "not the country's province";
        }
        if (provinces.controller[province] != country) {
            return "occupied";
        }
        if (besieged(state.sieges, province)) {
            return "besieged";
        }
        if (queued(state.recruits, province) > std::numeric_limits<std::uint8_t>::max()) {
            return "the province's queue is full";
        }
        if (state.money.cash[account(country, Account::treasury)] < recruit_cost(state, content, country, type)) {
            return "not enough money in the treasury";
        }
        if (state.countries.manpower[country] < kFullRegiment) {
            return "not enough manpower";
        }
        bool full = true;
        for (const std::uint64_t word : state.recruits.alive) {
            full = full && word == ~std::uint64_t{ 0 };
        }
        if (full) {
            return "too many regiments ordered in the world";
        }
        return nullptr;
    }

    void recruit(WorldState& state, const Content& content, std::uint16_t country, std::uint16_t province,
                 std::uint8_t type)
    {
        Recruits& recruits = state.recruits;
        const std::uint32_t place = queued(recruits, province);
        std::uint16_t slot = 0;
        if (!take_slot(recruits.alive, &slot)) {
            return;   // why_no_recruit said there was one
        }
        ledger::pay(&state.money, account(country, Account::treasury), recruit_cost(state, content, country, type));
        state.countries.manpower[country] -= kFullRegiment;
        recruits.province[slot] = province;
        recruits.country[slot] = country;
        recruits.type[slot] = type;
        recruits.place[slot] = static_cast<std::uint8_t>(place);
        recruits.days[slot] = 0;
    }

    void Recruitment::day(WorldState& state, const Content& content, const Cache& cache, std::uint32_t day)
    {
        Recruits& recruits = state.recruits;
        const Provinces& provinces = state.provinces;
        // The queues lost: the province no longer the recruiter's, or occupied.
        work_.clear();
        for_each_alive(recruits.alive, [&](std::uint32_t r) {
            const std::uint16_t p = recruits.province[r];
            if (provinces.owner[p] != recruits.country[r] || provinces.controller[p] != recruits.country[r]) {
                work_.push_back(static_cast<std::uint16_t>(r));
            }
        });
        for (const std::uint16_t r : work_) {
            log::info("day {}: {} loses the regiment it was raising in province {}", day,
                      content.countries.tag_text(recruits.country[r]), recruits.province[r]);
            drop(recruits, r);
        }
        // A day on for the first of each queue not held by a siege; the ones done stand.
        work_.clear();
        for_each_alive(recruits.alive, [&](std::uint32_t r) {
            if (recruits.place[r] != 0 || recruitment_paused(state, recruits.province[r], recruits.country[r])) {
                return;
            }
            const int time = recruit_time(content, recruits.type[r]);
            if (recruits.days[r] < time) {
                ++recruits.days[r];
            }
            if (recruits.days[r] >= time) {
                work_.push_back(static_cast<std::uint16_t>(r));
            }
        });
        for (const std::uint16_t r : work_) {
            std::uint16_t army = 0;
            if (!stand(state, content, cache, r, &army)) {
                continue;   // no slot: it waits, done
            }
            log::info("day {}: {} raises a regiment of {} in province {}: army {}:{}", day,
                      content.countries.tag_text(recruits.country[r]), content.regiment_types.key[recruits.type[r]],
                      recruits.province[r], army, state.armies.generation[army]);
            drop(recruits, r);
        }
    }
}
