#include "game/war_score.h"

#include <algorithm>

#include "game/armies.h"
#include "game/countries.h"
#include "game/economy/upkeep.h"
#include "game/occupation.h"
#include "game/war.h"

namespace mc
{
    namespace
    {
        // eu4.exe's numbers that are no defines.
        constexpr int kManyForts = 10;                   // an owner with this many forts: × 0.1 without a fort
        constexpr Fixed kLowestNoFort = 100;             // × 0.1
        constexpr Fixed kLowestValue = 1 * kFixedOne;    // a value's range
        constexpr Fixed kHighestValue = 100 * kFixedOne;

        // base_tax (a float >= 0 of the data) in Fixed, rounded to the thousandth.
        Fixed fixed_of(float x)
        {
            return static_cast<Fixed>(x * static_cast<float>(kFixedOne) + 0.5f);
        }

        // Σ value / total × 100 of the provinces `owner` owns and `occupier` controls (EU4's GetOccupiedScore),
        // whole points.
        int occupation(const WorldState& state, const Content& content, std::uint16_t occupier, std::uint16_t owner,
                       const CountryWarFacts& facts)
        {
            const Provinces& provinces = state.provinces;
            std::int64_t sum = 0;
            for (std::uint32_t p = 1; p < provinces.count; ++p) {
                if (provinces.owner[p] != owner || provinces.controller[p] != occupier) {
                    continue;
                }
                if (facts.total_value < 1) {
                    sum += kFixedOne;
                } else {
                    const Fixed value = province_value(state, content, static_cast<std::uint16_t>(p), facts);
                    sum += std::int64_t{ fx_div(value, std::int64_t{ facts.total_value } * kFixedOne) } * 100;
                }
            }
            return static_cast<int>(sum / kFixedOne);
        }

        // `winner` controls every province `loser` owns (it owns one at least): EU4's HasACompleteVictory.
        bool complete_victory(const WorldState& state, std::uint16_t winner, std::uint16_t loser)
        {
            const Provinces& provinces = state.provinces;
            bool any = false;
            for (std::uint32_t p = 1; p < provinces.count; ++p) {
                if (provinces.owner[p] != loser) {
                    continue;
                }
                if (provinces.controller[p] != winner) {
                    return false;
                }
                any = true;
            }
            return any;
        }
    }

    CountryWarFacts country_war_facts(const WorldState& state, const Content& content, std::uint16_t country)
    {
        CountryWarFacts facts;
        const Provinces& provinces = state.provinces;
        facts.seat = seat(state, country);
        if (country == 0) {
            return facts;
        }
        for (std::uint32_t p = 1; p < provinces.count; ++p) {
            if (provinces.owner[p] != country) {
                continue;
            }
            facts.development += fixed_of(provinces.base_tax[p]);
            if (fort_level(state, content, static_cast<std::uint16_t>(p)) >= 1) {
                ++facts.forts;
            }
        }
        std::int64_t total = 0;
        for (std::uint32_t p = 1; p < provinces.count; ++p) {
            if (provinces.owner[p] == country) {
                total += province_value(state, content, static_cast<std::uint16_t>(p), facts);
            }
        }
        facts.total_value = static_cast<std::int32_t>(total / kFixedOne);
        return facts;
    }

    Fixed province_value(const WorldState& state, const Content& content, std::uint16_t province,
                         const CountryWarFacts& owner)
    {
        const WarDefines& d = content.war;
        Fixed value = d.province_value_base +
                      std::min(fixed_of(state.provinces.base_tax[province]), d.province_value_cap);
        if (owner.seat == province) {
            value += fx_mul(value, d.capital_value_mult - kFixedOne);
        }
        if (fort_level(state, content, province) < 1) {
            Fixed share = kLowestNoFort;
            if (owner.forts < kManyForts) {
                // r = forts × ratio × 2 / development, -0.001 at no development (eu4.exe's divisor 0 → -1).
                Fixed r = -1;
                if (owner.development != 0) {
                    r = fx_div(fx_mul(std::int64_t{ owner.forts } * kFixedOne, d.fort_per_dev_ratio) * 2,
                               owner.development);
                }
                if (r < kFixedOne) {
                    share = static_cast<Fixed>((std::int64_t{ 4 * kFixedOne } - 3 * std::int64_t{ r }) * 100 /
                                               kFixedOne);
                }
            }
            value = fx_mul(value, share);
        }
        return std::clamp(value, kLowestValue, kHighestValue);
    }

    Fixed battle_score(const WarDefines& defines, Fixed loser_force_limit, std::int64_t losses)
    {
        const std::int64_t regiments = losses / 1000;
        const std::int64_t size = std::max<std::int64_t>(std::int64_t{ loser_force_limit } * 2, regiments * 2000);
        std::int64_t score = -1;
        if (size != 0) {
            const std::int64_t share = regiments * kFixedOne * kFixedOne / size;
            score = share * (std::int64_t{ defines.warscore_max_from_battles } / 2 * kFixedOne) / kFixedOne;
        }
        return static_cast<Fixed>(std::max<std::int64_t>(score, 100));
    }

    Fixed add_battle_score(WorldState& state, const Content& content, std::uint16_t winner, std::uint16_t loser,
                           std::int64_t losses)
    {
        const int war = war_between(state.wars, winner, loser);
        if (war < 0) {
            return 0;
        }
        const Fixed limit = fx_from_int(static_cast<std::int32_t>(force_limit(state, content, loser)));
        const Fixed score = battle_score(content.war, limit, losses);
        const auto w = static_cast<std::size_t>(war);
        state.wars.battle_score[w] += state.wars.attacker[w] == winner ? score : -score;
        return score;
    }

    WarScore war_score(const WorldState& state, const Content& content, std::uint16_t war)
    {
        WarScore s;
        if (!slot_alive(state.wars.alive, war)) {
            return s;
        }
        const WarDefines& d = content.war;
        const std::uint16_t attacker = state.wars.attacker[war];
        const std::uint16_t defender = state.wars.defender[war];
        s.battles = std::clamp(fx_to_int(state.wars.battle_score[war]), -d.warscore_max_from_battles,
                               d.warscore_max_from_battles);
        s.attacker_occupation = occupation(state, content, attacker, defender,
                                           country_war_facts(state, content, defender));
        s.defender_occupation = occupation(state, content, defender, attacker,
                                           country_war_facts(state, content, attacker));
        // The defender's complete victory is tested first, as in eu4.exe.
        if (complete_victory(state, defender, attacker)) {
            s.complete = -1;
            s.total = -d.max_warscore;
            return s;
        }
        if (complete_victory(state, attacker, defender)) {
            s.complete = 1;
            s.total = d.max_warscore;
            return s;
        }
        const int total = std::clamp(s.battles, -d.max_warscore, d.max_warscore) - s.defender_occupation +
                          s.attacker_occupation;
        s.total = std::clamp(total, 1 - d.max_warscore, d.max_warscore - 1);
        return s;
    }

    int war_score_of(const WorldState& state, const Content& content, std::uint16_t country, std::uint16_t enemy)
    {
        const int war = war_between(state.wars, country, enemy);
        if (war < 0) {
            return 0;
        }
        const WarScore s = war_score(state, content, static_cast<std::uint16_t>(war));
        return state.wars.attacker[static_cast<std::size_t>(war)] == country ? s.total : -s.total;
    }
}
