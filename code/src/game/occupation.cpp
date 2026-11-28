#include "game/occupation.h"

#include <algorithm>
#include <cmath>

#include "core/map_data.h"
#include "core/random.h"
#include "game/armies.h"
#include "game/countries.h"
#include "game/modifiers.h"
#include "game/war.h"

namespace mc
{
    namespace
    {
        constexpr std::uint32_t kSiegeDice = 16;   // the die's purpose (core/random.h; the battle's are 1 and 2)
        // TickSiegePhase's hard-coded numbers: a roll + impact of this or less changes nothing; a breach gives 2
        // progress; the garrison's losses, ‰ of its men: a breach and each result (supplies, food, water shortage,
        // the defenders desert).
        constexpr int kStatusQuoAtMost = 4;
        constexpr int kBreachProgress = 2;
        constexpr std::int64_t kLossBreach = 50;
        constexpr std::int64_t kLossSupplies = 10;
        constexpr std::int64_t kLossFood = 30;
        constexpr std::int64_t kLossWater = 50;
        constexpr std::int64_t kLossDesertion = 100;
        // A garrison below siege_garrison_surrender makes the roll this: a surrender whatever the impact.
        constexpr int kGarrisonGone = 99;

        int level_at(const WorldState& state, const Content& content, std::uint16_t province, std::uint32_t seat)
        {
            int level = state.provinces.fort[province];
            if (state.provinces.owner[province] != 0 && seat == province) {
                level += content.combat.capital_fort_level;
            }
            return level;
        }

        std::int64_t needed_at(const WorldState& state, const Content& content, std::uint16_t province, int level)
        {
            const CombatDefines& d = content.combat;
            std::int64_t needed = d.siege_min_men;
            if (level > 0 && state.provinces.garrison[province] > 0) {
                const std::int64_t force = fx_mul(max_garrison(content, level), d.siege_force_needed_multiplier);
                needed = std::max(needed, force);
            }
            return needed;
        }
    }

    int fort_level(const WorldState& state, const Content& content, std::uint16_t province)
    {
        return level_at(state, content, province, seat(state, state.provinces.owner[province]));
    }

    std::int32_t max_garrison(const Content& content, int fort_level)
    {
        return fort_level > 0 ? fort_level * content.combat.garrison_size : 0;
    }

    std::int64_t siege_men_needed(const WorldState& state, const Content& content, std::uint16_t province)
    {
        return needed_at(state, content, province, fort_level(state, content, province));
    }

    bool may_besiege(const WorldState& state, const AtWar& at_war, std::uint16_t country, std::uint16_t province)
    {
        const std::uint16_t owner = state.provinces.owner[province];
        const std::uint16_t controller = state.provinces.controller[province];
        return country != 0 && owner != 0 && controller != 0 && controller != country &&
               mc::at_war(at_war, country, controller) && (owner == country || mc::at_war(at_war, country, owner));
    }

    bool has_port(const MapData& map, std::uint16_t province)
    {
        const std::size_t i = std::size_t{ province } * 8 + 6;   // PPOS: city, unit, text, port
        return i < map.positions.size() && !std::isnan(map.positions[i]);
    }

    void fill_garrisons(WorldState& state, const Content& content)
    {
        Provinces& provinces = state.provinces;
        for (std::uint32_t p = 1; p < provinces.count; ++p) {
            provinces.garrison[p] = max_garrison(content, fort_level(state, content, static_cast<std::uint16_t>(p)));
        }
    }

    namespace
    {
        // The commander's pick (EU4's CalcHighestCommander, as the battle's: game/combat.h): the general with the
        // most fire + shock + 1; an equal one does not replace the one of a lower army slot.
        void consider_general(const Leaders& leaders, std::uint16_t army_leader, Besiegers* b)
        {
            if (army_leader == 0) {
                return;
            }
            const int l = army_leader - 1;
            const int score = leaders.fire[l] + leaders.shock[l] + 1;
            if (score > b->commander_score) {
                b->commander_score = score;
                b->commander = l;
            }
        }

        // The army in `slot` may besiege where it is: owned, standing (not marching) and not retreating.
        bool standing(const Armies& armies, std::uint32_t slot)
        {
            return armies.owner[slot] != 0 && armies.next[slot] == 0 &&
                   !((armies.retreating[slot / 64] >> (slot % 64)) & 1u);
        }
    }

    Besiegers besiegers(const WorldState& state, const Content& content, std::uint16_t slot)
    {
        Besiegers out;
        const Armies& armies = state.armies;
        const Regiments& regiments = state.regiments;
        const std::uint16_t province = state.sieges.province[slot];
        const std::uint16_t country = state.sieges.country[slot];
        for_each_alive(armies.alive, [&](std::uint32_t a) {
            if (armies.owner[a] != country || armies.province[a] != province || !standing(armies, a)) {
                return;
            }
            consider_general(state.leaders, armies.leader[a], &out);
            for_each_alive(regiments.alive, [&](std::uint32_t r) {
                if (regiments.army[r] != a) {
                    return;
                }
                out.men += regiments.strength[r];
                if (content.regiment_category[regiments.type[r]] == RegimentCategory::artillery) {
                    out.artillery += regiments.strength[r];
                }
            });
        });
        return out;
    }

    SiegeNumbers siege_numbers(const WorldState& state, const Content& content, const Cache& cache, bool port,
                               std::uint16_t slot, const Besiegers& b)
    {
        const CombatDefines& d = content.combat;
        const CountryModifiers& sums = cache.country_modifiers;
        const Provinces& provinces = state.provinces;
        const std::uint16_t province = state.sieges.province[slot];
        const std::uint16_t country = state.sieges.country[slot];
        const std::uint16_t controller = provinces.controller[province];
        SiegeNumbers n;
        n.fort = fort_level(state, content, province);
        n.max_garrison = max_garrison(content, n.fort);
        n.needed = needed_at(state, content, province, n.fort);
        const int highest = highest_fort(sums, country);
        n.progress_cap = d.siege_memory + fx_to_int(fx_mul(fx_from_int(highest), d.siege_memory_scaled));
        // GetDaysPerSiegePhase: days_per_siege_phase × max(0.1, 1 + the defender's fort defence - the besieger's
        // siege ability - the tactics' difference / 4, at most ±0.5; CalcTacticsDiff: × 250 / 1000, base tactics).
        {
            const Fixed tactics = fx_mul(sums.military_tactics[country] - sums.military_tactics[controller], 250);
            const Fixed factor = std::max<Fixed>(kFixedOne + fort_defence(sums, controller) -
                                                     siege_ability(sums, country) -
                                                     std::clamp<Fixed>(tactics, -500, 500),
                                                 100);
            n.phase_days = std::max(1, fx_to_int(fx_mul(fx_from_int(d.days_per_siege_phase), factor)));
        }
        if (n.fort == 0) {
            return n;   // no fort: no impact (it falls at its first phase all the same)
        }
        if (b.commander >= 0) {
            n.leader = state.leaders.siege[static_cast<std::size_t>(b.commander)];
        }
        // GetArtilleryValue: the artillery's regiments (1000 men each, any split) / (fort / 2 + 1), whole, capped.
        {
            const std::int64_t value = b.artillery / (n.fort / 2 + 1);   // in men: 1000 = one level
            n.artillery = static_cast<int>(std::min<std::int64_t>(artillery_vs_fort(sums, country), value / 1000));
            if (value > 0 && n.artillery == 0) {
                n.artillery = 1;
            }
            n.artillery = std::max(n.artillery, 0);
        }
        n.blockade = port ? -d.siege_unblockaded_penalty : 0;
        n.obsolete = std::max(0, (highest - std::max(n.fort, 2)) / 2);
        n.low_garrison = provinces.garrison[province] < n.max_garrison / 2 ? 1 : 0;
        n.impact = provinces.siege_progress[province] + n.leader + state.sieges.breaches[slot] + n.artillery +
                   n.blockade - n.fort + n.obsolete + n.low_garrison;
        return n;
    }

    SiegeOutcome siege_outcome(const CombatDefines& d, const SiegeNumbers& n, int breaches, std::int32_t garrison,
                               int roll)
    {
        SiegeOutcome out;
        int total = roll + n.impact;
        if (garrison < d.siege_garrison_surrender) {
            total = kGarrisonGone;
        }
        if (roll == 1 && total >= d.siege_win) {
            roll = 2;   // a surrender beats a disease
        }
        out.roll = roll;
        if (roll == 1) {
            out.result = SiegeResult::disease;
        } else if (n.artillery / 2 + roll + n.obsolete / 2 >= d.siege_dice_sides && breaches < d.max_breach &&
                   total < d.siege_win) {
            out.result = SiegeResult::breach;
            out.progress = kBreachProgress;
            out.loss = kLossBreach;
        } else if (total > kStatusQuoAtMost) {
            if (total < d.siege_win - 8) {
                out = { roll, SiegeResult::supplies, false, d.siege_bonus_supplies_shortage, kLossSupplies };
            } else if (total < d.siege_win - 6) {
                out = { roll, SiegeResult::food, false, d.siege_bonus_food_shortage, kLossFood };
            } else if (total < d.siege_win - 4) {
                out = { roll, SiegeResult::water, false, d.siege_bonus_water_shortage, kLossWater };
            } else if (total < d.siege_win) {
                out = { roll, SiegeResult::desertion, false, d.siege_bonus_defenders_desert, kLossDesertion };
            } else {
                out.surrender = true;
            }
        }
        return out;
    }

    SiegeChances siege_chances(const CombatDefines& d, const SiegeNumbers& n, int breaches, std::int32_t garrison)
    {
        SiegeChances chances;
        chances.sides = d.siege_dice_sides;
        for (int roll = 1; roll <= d.siege_dice_sides; ++roll) {
            const SiegeOutcome out = siege_outcome(d, n, breaches, n.fort > 0 ? garrison : 0, roll);
            if (out.surrender) {
                ++chances.surrender;
            } else {
                ++chances.result[static_cast<std::size_t>(out.result)];
            }
        }
        return chances;
    }

    void Occupation::build(const MapData& map, const Content& content)
    {
        province_count_ = map.count;
        stamp_ = 0;
        const std::size_t n = std::size_t{ map.count } + 1;
        port_.assign(n, 0);
        for (std::uint32_t p = 1; p < map.count; ++p) {
            port_[p] = has_port(map, static_cast<std::uint16_t>(p)) ? 1 : 0;
        }
        battle_mark_.assign(n, 0);
        siege_mark_.assign(n, 0);
        siege_at_.assign(n, 0);
        candidate_mark_.assign(n, 0);
        candidate_.assign(n, 0);
        candidate_men_.assign(n, 0);
        candidates_.assign(n, 0);
        army_men_.assign(kArmySlots, 0);
        army_artillery_.assign(kArmySlots, 0);
        besiegers_.assign(kSiegeSlots, Besiegers{});
        seat_.assign(content.countries.size(), 0);
    }

    void Occupation::begin(WorldState& state, std::uint16_t province, std::uint16_t country)
    {
        Sieges& sieges = state.sieges;
        std::uint16_t slot = 0;
        if (!take_slot(sieges.alive, &slot)) {
            return;   // every slot taken: no siege (one a province, so only with a map of more provinces)
        }
        sieges.province[slot] = province;
        sieges.country[slot] = country;
        siege_mark_[province] = stamp_;
        siege_at_[province] = slot;
    }

    void Occupation::end(WorldState& state, std::uint16_t slot)
    {
        Sieges& sieges = state.sieges;
        siege_mark_[sieges.province[slot]] = 0;
        const std::uint16_t generation = sieges.generation[slot];
#define MC_CLEAR_SLOT(table, type, name, start) sieges.name[slot] = type{};
        MC_SIEGE_COLUMNS(MC_CLEAR_SLOT)
#undef MC_CLEAR_SLOT
        sieges.generation[slot] = static_cast<std::uint16_t>(generation + 1);
        free_slot(sieges.alive, slot);
    }

    void Occupation::day(WorldState& state, const Content& content, const Cache& cache, std::uint32_t seed,
                         std::uint32_t day)
    {
        ++stamp_;
        const Armies& armies = state.armies;
        const Regiments& regiments = state.regiments;
        Sieges& sieges = state.sieges;
        Provinces& provinces = state.provinces;

        for_each_alive(state.battles.alive,
                       [&](std::uint32_t b) { battle_mark_[state.battles.list[b].province] = stamp_; });
        for_each_alive(sieges.alive, [&](std::uint32_t s) {
            siege_mark_[sieges.province[s]] = stamp_;
            siege_at_[sieges.province[s]] = static_cast<std::uint16_t>(s);
            besiegers_[s] = {};
        });
        // The armies' men and artillery today, one pass over the regiments.
        const std::size_t used = used_slots(armies.alive);
        std::fill_n(army_men_.begin(), used, 0);
        std::fill_n(army_artillery_.begin(), used, 0);
        for_each_alive(regiments.alive, [&](std::uint32_t r) {
            const std::uint16_t a = regiments.army[r];
            army_men_[a] += regiments.strength[r];
            if (content.regiment_category[regiments.type[r]] == RegimentCategory::artillery) {
                army_artillery_[a] += regiments.strength[r];
            }
        });
        // The besiegers of each siege, and the provinces where a siege may begin: the first standing army (by slot)
        // whose country may besiege a province with no siege and no battle names that country.
        std::uint32_t candidates = 0;
        for_each_alive(armies.alive, [&](std::uint32_t a) {
            if (!standing(armies, a)) {
                return;
            }
            const std::uint16_t p = armies.province[a];
            const std::uint16_t owner = armies.owner[a];
            if (siege_mark_[p] == stamp_) {
                const std::uint16_t s = siege_at_[p];
                if (sieges.country[s] == owner) {
                    Besiegers& b = besiegers_[s];
                    b.men += army_men_[a];
                    b.artillery += army_artillery_[a];
                    consider_general(state.leaders, armies.leader[a], &b);
                }
                return;
            }
            if (battle_mark_[p] == stamp_ || candidate_mark_[p] == stamp_ ||
                !may_besiege(state, cache.at_war, owner, p)) {
                return;
            }
            candidate_mark_[p] = stamp_;
            candidate_[p] = owner;
            candidate_men_[p] = 0;
            candidates_[candidates++] = p;
        });
        if (candidates > 0) {
            for_each_alive(armies.alive, [&](std::uint32_t a) {
                const std::uint16_t p = armies.province[a];
                if (standing(armies, a) && candidate_mark_[p] == stamp_ && candidate_[p] == armies.owner[a]) {
                    candidate_men_[p] += army_men_[a];
                }
            });
        }

        // The sieges going on, in slot order.
        for_each_alive(sieges.alive, [&](std::uint32_t s) {
            const auto slot = static_cast<std::uint16_t>(s);
            const std::uint16_t p = sieges.province[s];
            if (!may_besiege(state, cache.at_war, sieges.country[s], p) ||
                besiegers_[s].men < siege_men_needed(state, content, p)) {
                end(state, slot);
                return;
            }
            if (battle_mark_[p] == stamp_) {
                return;   // the battle holds the siege's days (EU4's DailyUpdate returns while one goes on)
            }
            ++sieges.days[s];
            const SiegeNumbers n = siege_numbers(state, content, cache, port_[p] != 0, slot, besiegers_[s]);
            if (sieges.days[s] >= n.phase_days) {
                sieges.days[s] = 0;
                phase(state, content, cache, slot, seed, day);
            }
        });
        // The new ones, in the order found; their days count from tomorrow.
        for (std::uint32_t k = 0; k < candidates; ++k) {
            const std::uint16_t p = candidates_[k];
            if (siege_mark_[p] != stamp_ && candidate_men_[p] >= siege_men_needed(state, content, p)) {
                begin(state, p, candidate_[p]);
            }
        }
        // The progress of a province no one besieges goes down a point a day.
        for (std::uint32_t p = 1; p < province_count_; ++p) {
            if (provinces.siege_progress[p] > 0 && siege_mark_[p] != stamp_) {
                --provinces.siege_progress[p];
            }
        }
    }

    // TickSiegePhase: the die, then one of a disease, a breach, a result of the table, a surrender.
    void Occupation::phase(WorldState& state, const Content& content, const Cache& cache, std::uint16_t slot,
                           std::uint32_t seed, std::uint32_t day)
    {
        const CombatDefines& d = content.combat;
        Sieges& sieges = state.sieges;
        Provinces& provinces = state.provinces;
        const std::uint16_t p = sieges.province[slot];
        const SiegeNumbers n = siege_numbers(state, content, cache, port_[p] != 0, slot, besiegers_[slot]);
        const std::uint32_t entity = std::uint32_t{ slot } | std::uint32_t{ sieges.generation[slot] } << 16;
        const int roll = static_cast<int>(random_below(seed, day, entity, kSiegeDice,
                                                       static_cast<std::uint32_t>(d.siege_dice_sides))) +
                         1;
        const std::int32_t garrison = n.fort > 0 ? provinces.garrison[p] : 0;
        const SiegeOutcome out = siege_outcome(d, n, sieges.breaches[slot], garrison, roll);
        ++sieges.phases[slot];
        sieges.roll[slot] = static_cast<std::uint8_t>(out.roll);
        sieges.result[slot] = out.result;
        if (out.result == SiegeResult::disease) {
            // Every regiment of the besiegers loses siege_disease_impact of its men (CSubUnit::LoseStrength).
            const Armies& armies = state.armies;
            Regiments& regiments = state.regiments;
            const std::uint16_t country = sieges.country[slot];
            for_each_alive(regiments.alive, [&](std::uint32_t r) {
                const std::uint16_t a = regiments.army[r];
                if (armies.owner[a] == country && armies.province[a] == p && standing(armies, a)) {
                    regiments.strength[r] -= fx_mul(regiments.strength[r], d.siege_disease_impact);
                }
            });
        }
        if (out.result == SiegeResult::breach) {
            ++sieges.breaches[slot];
        }
        if (out.surrender) {
            // The besieger controls the province (its owner's again, if it is the owner): the progress is gone, the
            // fort keeps a token garrison.
            provinces.controller[p] = sieges.country[slot];
            provinces.siege_progress[p] = 0;
            provinces.garrison[p] = n.fort > 0 ? std::min(d.garrison_after_capture, n.max_garrison) : 0;
            end(state, slot);
            return;
        }
        provinces.siege_progress[p] = static_cast<std::uint8_t>(
            std::clamp(provinces.siege_progress[p] + out.progress, 0, std::max(n.progress_cap, 0)));
        if (n.fort > 0) {
            const std::int64_t left = garrison - garrison * out.loss / 1000;
            provinces.garrison[p] = static_cast<std::int32_t>(std::clamp<std::int64_t>(left, 0, n.max_garrison));
        }
    }

    void Occupation::month(WorldState& state, const Content& content)
    {
        ++stamp_;
        for_each_alive(state.sieges.alive, [&](std::uint32_t s) { siege_mark_[state.sieges.province[s]] = stamp_; });
        for (std::uint32_t c = 1; c < state.countries.count && c < seat_.size(); ++c) {
            seat_[c] = seat(state, static_cast<std::uint16_t>(c));
        }
        Provinces& provinces = state.provinces;
        const Fixed growth = content.combat.garrison_growth;
        for (std::uint32_t p = 1; p < provinces.count; ++p) {
            const std::uint16_t owner = provinces.owner[p];
            const std::uint32_t seat_of_owner = owner < seat_.size() ? seat_[owner] : 0;
            const int level = level_at(state, content, static_cast<std::uint16_t>(p), seat_of_owner);
            const std::int32_t most = max_garrison(content, level);
            std::int32_t& garrison = provinces.garrison[p];
            if (siege_mark_[p] != stamp_ && garrison < most) {
                garrison = std::min(most, garrison + fx_mul(most, growth));
            }
            garrison = std::min(garrison, most);
        }
        // The marks of the month are not the day's: the next day() takes a new stamp and marks its own.
    }
}
