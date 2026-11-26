#include "game/combat.h"

#include <algorithm>
#include <array>
#include <bit>
#include <limits>

#include "core/log.h"
#include "core/map_data.h"
#include "core/random.h"
#include "game/armies.h"
#include "game/modifiers.h"
#include "game/movement.h"

// The order of operations and the integer arithmetic follow eu4.exe 1.37.5 as BattleSim reproduces it
// (C:\Dev\Projects\eu4\BattleSim, src/core/Battle.cpp; addresses there). Not yet here: generals, terrain and crossings
// (the dice's global modifier is 0), artillery's back line (artillery stays in the reserve, as EU4's reserve never
// sends it to the front), the cavalry's insufficient support, combat ability and the damage modifiers (all 0).

namespace mc
{
    namespace
    {
        constexpr Fixed kBroken = 10;                        // morale >= 0.010 is able to fight (EU4: 9 < morale)
        constexpr int kCenter = kMaxCombatWidth / 2 + 1;     // EU4's GetCombatCenter
        constexpr std::uint32_t kDiceAttacker = 1;           // the dice's purposes (core/random.h)
        constexpr std::uint32_t kDiceDefender = 2;

        // The order every loop of EU4's combat visits the front: centre, centre - 1, centre + 1, centre - 2, ...
        struct SlotOrder
        {
            std::array<std::int8_t, kMaxCombatWidth> out{};
            constexpr SlotOrder()
            {
                for (int i = 0; i < kMaxCombatWidth; ++i) {
                    const int sign = (i & 1) == 0 ? 1 : -1;
                    out[static_cast<std::size_t>(i)] = static_cast<std::int8_t>((i + 1) / 2 * sign + kCenter);
                }
            }
        };
        constexpr SlotOrder kOrder;

        int order_at(int i) { return kOrder.out[static_cast<std::size_t>(i)]; }

        // EU4's CSubUnit::GetNumOfRanks: 4 above 75% of a full regiment (1000 men), 3 above 50%, 2 above 25%, else 1.
        int ranks(std::int32_t strength)
        {
            return strength > 750 ? 4 : strength > 500 ? 3 : strength > 250 ? 2 : 1;
        }

        bool alive(const Regiments& regiments, std::uint16_t r)
        {
            return regiments.strength[r] > 0 && regiments.morale[r] >= kBroken;
        }

        RegimentCategory category_of(const Content& content, const Regiments& regiments, std::uint16_t r)
        {
            const std::uint8_t type = regiments.type[r];
            return type < content.regiment_category.size() ? content.regiment_category[type]
                                                           : RegimentCategory::infantry;
        }

        bool in_front(std::uint8_t place) { return place != kPlaceNone && place <= kFrontSlots; }

        // The lowest free slot of a mask, or false.
        bool take_battle_slot(Column<std::uint64_t>& alive_mask, std::uint32_t* slot)
        {
            for (std::uint32_t w = 0; w < alive_mask.size(); ++w) {
                if (alive_mask[w] != ~std::uint64_t{ 0 }) {
                    const int bit = std::countr_one(alive_mask[w]);
                    alive_mask[w] |= std::uint64_t{ 1 } << bit;
                    *slot = w * 64 + static_cast<std::uint32_t>(bit);
                    return true;
                }
            }
            return false;
        }

        bool retreating(const Armies& armies, std::uint32_t a) { return (armies.retreating[a / 64] >> (a % 64)) & 1u; }
        void set_retreating(Armies& armies, std::uint32_t a, bool on)
        {
            const std::uint64_t bit = std::uint64_t{ 1 } << (a % 64);
            armies.retreating[a / 64] = on ? (armies.retreating[a / 64] | bit) : (armies.retreating[a / 64] & ~bit);
        }

        // The side of a regiment's army in its battle: 0 the attacker's country, 1 the defender's.
        int side_of(const WorldState& state, std::uint32_t battle, std::uint16_t r)
        {
            return state.armies.owner[state.regiments.army[r]] == state.battles.attacker[battle] ? 0 : 1;
        }
    }

    void Combat::build(const MapData& map)
    {
        province_count_ = map.count;
        const std::size_t sides = std::size_t{ kBattleSlots } * 2;
        army_offsets_.assign(padded(std::size_t{ province_count_ } + 1), 0);
        army_list_.assign(kArmySlots, 0);
        regiment_offsets_.assign(padded(std::size_t{ kArmySlots } + 1), 0);
        regiment_list_.assign(kRegimentSlots, 0);
        army_regiments_.assign(kArmySlots, 0);
        province_battle_.assign(padded(province_count_), 0);
        fill_.assign(padded(std::max({ std::size_t{ province_count_ } + 1, std::size_t{ kArmySlots } + 1, sides })), 0);
        front_.assign(sides * kFrontSlots, kEmpty);
        // Room: a side's reserve at the day's start, as many again (the first day draws everyone up and sends the
        // rest back to the end), and a front line (the regiments past the width go back to the reserve) plus one (the
        // one without a target).
        const std::size_t pool = 2 * std::size_t{ kRegimentSlots } + sides * (kFrontSlots + 1);
        reserve_regiment_.assign(pool, 0);
        reserve_queue_.assign(pool, 0);
        reserve_begin_.assign(sides, 0);
        reserve_end_.assign(sides, 0);
        reserve_room_.assign(sides, 0);
        damage_men_.assign(kRegimentSlots, 0);
        damage_morale_.assign(kRegimentSlots, 0);
        side_discipline_.assign(sides, 0);
        side_tactics_.assign(sides, 0);
        side_max_morale_.assign(sides, 0);
        side_weapon_.assign(sides * 4, 0);
        side_men_.assign(sides, 0);
        side_morale_.assign(sides, 0);
        side_regiments_.assign(sides, 0);
        side_fit_morale_.assign(sides, 0);
        side_fit_.assign(sides, 0);
        width_.assign(kBattleSlots, 0);
        centre_.assign(kRegimentSlots, 0);
        flank_.assign(kRegimentSlots, 0);
    }

    // ------------------------------------------------------------------------------------------------ the day

    void Combat::day(WorldState& state, const Content& content, const CountryModifiers& sums, Movement& movement,
                     std::uint32_t seed, std::uint32_t day)
    {
        Armies& armies = state.armies;
        Regiments& regiments = state.regiments;
        // A retreat ends where its march does: the army stands again.
        for (std::uint32_t w = 0; w < armies.retreating.size(); ++w) {
            for (std::uint64_t bits = armies.retreating[w]; bits != 0; bits &= bits - 1) {
                const std::uint32_t a = w * 64 + static_cast<std::uint32_t>(std::countr_zero(bits));
                if (armies.next[a] == 0) {
                    set_retreating(armies, a, false);
                }
            }
        }
        index(state);
        meet(state, content, movement, day);
        if (used_slots(state.battles.alive) == 0) {
            return;
        }
        rebuild(state, content, sums);
        tally(state);

        // Every battle up to its shots, in slot order (a battle that ends before the first shot fires none).
        std::array<std::uint64_t, kBattleSlots / 64> fought{};
        for_each_alive(state.battles.alive, [&](std::uint32_t b) {
            if (fight(state, content, movement, b, seed, day)) {
                fought[b / 64] |= std::uint64_t{ 1 } << (b % 64);
            }
        });

        // The day's damage lands on every regiment at once (EU4's ApplyDamage): a dense pass, no branches.
        const std::size_t used = used_slots(regiments.alive);
        for (std::size_t r = 0; r < used; ++r) {
            regiments.strength[r] = std::max(regiments.strength[r] - damage_men_[r], 0);
            regiments.morale[r] = std::max(regiments.morale[r] - damage_morale_[r], 0);
            damage_men_[r] = 0;
            damage_morale_[r] = 0;
        }
        // Regiments with no men left are gone (EU4's DeleteEmptySubUnits), and armies with no regiments left.
        for_each_alive(regiments.alive, [&](std::uint32_t r) {
            if (regiments.place[r] == kPlaceNone || regiments.strength[r] != 0) {
                return;
            }
            const std::uint16_t army = regiments.army[r];
            if (in_front(regiments.place[r])) {
                const std::uint32_t b = armies.battle[army] - 1u;
                front(b, side_of(state, b, static_cast<std::uint16_t>(r)), regiments.place[r] - 1) = kEmpty;
            }
            --army_regiments_[army];
            remove_regiment(regiments, static_cast<std::uint16_t>(r));
        });
        for_each_alive(armies.alive, [&](std::uint32_t a) {
            if (armies.battle[a] != 0 && army_regiments_[a] == 0) {
                destroy_army(state, static_cast<std::uint16_t>(a));
            }
        });
        tally(state);

        for (std::uint32_t w = 0; w < fought.size(); ++w) {
            for (std::uint64_t bits = fought[w]; bits != 0; bits &= bits - 1) {
                settle(state, content, movement, w * 64 + static_cast<std::uint32_t>(std::countr_zero(bits)));
            }
        }
    }

    void Combat::index(const WorldState& state)
    {
        const Armies& armies = state.armies;
        const Regiments& regiments = state.regiments;
        // Armies by province (a counting sort: slot order within a province).
        const std::uint32_t n = province_count_;
        std::fill(fill_.begin(), fill_.begin() + n + 1, 0u);
        for_each_alive(armies.alive, [&](std::uint32_t a) {
            if (armies.province[a] < n) {
                ++fill_[armies.province[a]];
            }
        });
        std::uint32_t at = 0;
        for (std::uint32_t p = 0; p < n; ++p) {
            army_offsets_[p] = at;
            at += fill_[p];
            fill_[p] = army_offsets_[p];
        }
        army_offsets_[n] = at;
        for_each_alive(armies.alive, [&](std::uint32_t a) {
            if (armies.province[a] < n) {
                army_list_[fill_[armies.province[a]]++] = static_cast<std::uint16_t>(a);
            }
        });
        // Regiments by army.
        std::fill(army_regiments_.begin(), army_regiments_.end(), 0u);
        for_each_alive(regiments.alive, [&](std::uint32_t r) { ++army_regiments_[regiments.army[r]]; });
        at = 0;
        for (std::uint32_t a = 0; a < kArmySlots; ++a) {
            regiment_offsets_[a] = at;
            fill_[a] = at;
            at += army_regiments_[a];
        }
        regiment_offsets_[kArmySlots] = at;
        for_each_alive(regiments.alive, [&](std::uint32_t r) {
            regiment_list_[fill_[regiments.army[r]]++] = static_cast<std::uint16_t>(r);
        });
    }

    void Combat::meet(WorldState& state, const Content& content, Movement& movement, std::uint32_t day)
    {
        Armies& armies = state.armies;
        Battles& battles = state.battles;
        std::fill(province_battle_.begin(), province_battle_.end(), std::uint16_t{ 0 });
        for_each_alive(battles.alive, [&](std::uint32_t b) {
            province_battle_[battles.province[b]] = static_cast<std::uint16_t>(b + 1);
        });
        // An army can fight: it has an owner and does not retreat.
        auto fighting = [&](std::uint16_t a) { return armies.owner[a] != 0 && !retreating(armies, a); };
        for (std::uint32_t p = 1; p < province_count_; ++p) {
            const std::uint32_t lo = army_offsets_[p];
            const std::uint32_t hi = army_offsets_[p + 1];
            if (hi - lo < 1) {
                continue;
            }
            if (province_battle_[p] == 0) {
                // A new battle where armies of two countries meet. The defender: the country of the first army (in
                // slot order) that was here before today, else of the first; the attacker: the first other country
                // that came in today, else the first other. A third country waits for the battle's end.
                std::uint16_t defender = 0;
                std::uint16_t attacker = 0;
                for (std::uint32_t k = lo; k < hi && defender == 0; ++k) {
                    const std::uint16_t a = army_list_[k];
                    if (fighting(a) && !movement.arrived_today(a)) {
                        defender = armies.owner[a];
                    }
                }
                for (std::uint32_t k = lo; k < hi && defender == 0; ++k) {
                    if (fighting(army_list_[k])) {
                        defender = armies.owner[army_list_[k]];
                    }
                }
                for (std::uint32_t k = lo; k < hi && attacker == 0; ++k) {
                    const std::uint16_t a = army_list_[k];
                    if (fighting(a) && armies.owner[a] != defender && movement.arrived_today(a)) {
                        attacker = armies.owner[a];
                    }
                }
                for (std::uint32_t k = lo; k < hi && attacker == 0; ++k) {
                    const std::uint16_t a = army_list_[k];
                    if (fighting(a) && armies.owner[a] != defender) {
                        attacker = armies.owner[a];
                    }
                }
                if (attacker == 0) {
                    continue;
                }
                std::uint32_t b = 0;
                if (!take_battle_slot(battles.alive, &b)) {
                    log::warn("day {}: no battle slot left for province {}", day, p);
                    continue;
                }
                battles.province[b] = static_cast<std::uint16_t>(p);
                battles.attacker[b] = attacker;
                battles.defender[b] = defender;
                battles.duration[b] = 0;
                battles.phase_day[b] = kNotDeployed;
                battles.phase[b] = kPhaseFire;
                battles.queue[b] = 0;
                battles.joined_attacker[b] = 0;
                battles.joined_defender[b] = 0;
                province_battle_[p] = static_cast<std::uint16_t>(b + 1);
                log::info("day {}: battle in province {}: {} attacks {}", day, p, content.countries.tag_text(attacker),
                          content.countries.tag_text(defender));
            }
            // The armies of the battle's two countries that are not in it yet join it (EU4's TryToJoin), in slot
            // order.
            const std::uint32_t b = province_battle_[p] - 1u;
            for (std::uint32_t k = lo; k < hi; ++k) {
                const std::uint16_t a = army_list_[k];
                const std::uint16_t owner = armies.owner[a];
                if (fighting(a) && armies.battle[a] == 0 && (owner == battles.attacker[b] || owner == battles.defender[b])) {
                    join(state, movement, b, a);
                }
            }
        }
    }

    void Combat::join(WorldState& state, Movement& movement, std::uint32_t battle, std::uint16_t army)
    {
        Battles& battles = state.battles;
        Regiments& regiments = state.regiments;
        state.armies.battle[army] = static_cast<std::uint16_t>(battle + 1);
        movement.halt(state, army);
        // Its regiments go to the end of its side's reserve, in slot order (EU4's AddUnit), with the morale they have.
        const bool attacker = state.armies.owner[army] == battles.attacker[battle];
        for (std::uint32_t k = regiment_offsets_[army]; k < regiment_offsets_[army + 1]; ++k) {
            const std::uint16_t r = regiment_list_[k];
            regiments.place[r] = kPlaceReserve;
            regiments.queue[r] = battles.queue[battle]++;
            (attacker ? battles.joined_attacker : battles.joined_defender)[battle] += regiments.strength[r];
        }
    }

    void Combat::rebuild(const WorldState& state, const Content& content, const CountryModifiers& sums)
    {
        const Battles& battles = state.battles;
        const Regiments& regiments = state.regiments;
        // The lines from the places; the reserve's regiments counted per side.
        for_each_alive(battles.alive, [&](std::uint32_t b) {
            std::fill_n(front_.begin() + static_cast<std::ptrdiff_t>(side_index(b, 0) * kFrontSlots), 2 * kFrontSlots,
                        kEmpty);
            fill_[side_index(b, 0)] = 0;
            fill_[side_index(b, 1)] = 0;
        });
        for_each_alive(regiments.alive, [&](std::uint32_t r) {
            const std::uint8_t place = regiments.place[r];
            if (place == kPlaceNone) {
                return;
            }
            const std::uint32_t b = state.armies.battle[regiments.army[r]] - 1u;
            const int side = side_of(state, b, static_cast<std::uint16_t>(r));
            if (in_front(place)) {
                front(b, side, place - 1) = static_cast<std::uint16_t>(r);
            } else if (place == kPlaceReserve) {
                ++fill_[side_index(b, side)];
            }
        });
        // Each side's range of the pool, with its room; then its reserve in queue order.
        std::uint32_t at = 0;
        for_each_alive(battles.alive, [&](std::uint32_t b) {
            for (int side = 0; side < 2; ++side) {
                const std::size_t s = side_index(b, side);
                reserve_begin_[s] = at;
                reserve_end_[s] = at;
                reserve_room_[s] = 2 * fill_[s] + kFrontSlots + 1;
                at += reserve_room_[s];
            }
        });
        for_each_alive(regiments.alive, [&](std::uint32_t r) {
            if (regiments.place[r] != kPlaceReserve) {
                return;
            }
            const std::uint32_t b = state.armies.battle[regiments.army[r]] - 1u;
            const std::size_t s = side_index(b, side_of(state, b, static_cast<std::uint16_t>(r)));
            reserve_regiment_[reserve_end_[s]++] = static_cast<std::uint16_t>(r);
        });
        for_each_alive(battles.alive, [&](std::uint32_t b) {
            for (int side = 0; side < 2; ++side) {
                const std::size_t s = side_index(b, side);
                const auto first = reserve_regiment_.begin() + reserve_begin_[s];
                const auto last = reserve_regiment_.begin() + reserve_end_[s];
                // The queue numbers of a battle are unique: a full order.
                std::sort(first, last, [&](std::uint16_t x, std::uint16_t y) {
                    return regiments.queue[x] < regiments.queue[y];
                });
                for (std::uint32_t k = reserve_begin_[s]; k < reserve_end_[s]; ++k) {
                    reserve_queue_[k] = regiments.queue[reserve_regiment_[k]];
                }
            }
            // The sides' numbers for the day, and the width: the wider country's, within the defines' limit.
            const std::uint16_t countries[2] = { battles.attacker[b], battles.defender[b] };
            for (int side = 0; side < 2; ++side) {
                const std::size_t s = side_index(b, side);
                const std::uint16_t c = countries[side];
                side_discipline_[s] = discipline(sums, c);
                side_tactics_[s] = tactics(sums, c);
                side_max_morale_[s] = max_morale(sums, c);
                side_weapon_[s * 4 + 0] = weapon(sums, c, RegimentCategory::infantry, kPhaseFire);
                side_weapon_[s * 4 + 1] = weapon(sums, c, RegimentCategory::infantry, kPhaseShock);
                side_weapon_[s * 4 + 2] = weapon(sums, c, RegimentCategory::cavalry, kPhaseFire);
                side_weapon_[s * 4 + 3] = weapon(sums, c, RegimentCategory::cavalry, kPhaseShock);
            }
            const int width = std::max(combat_width(sums, countries[0]), combat_width(sums, countries[1]));
            width_[b] = static_cast<std::uint8_t>(std::clamp(width, 1, content.combat.max_combat_width));
        });
    }

    void Combat::tally(const WorldState& state)
    {
        const Regiments& regiments = state.regiments;
        for_each_alive(state.battles.alive, [&](std::uint32_t b) {
            for (int side = 0; side < 2; ++side) {
                const std::size_t s = side_index(b, side);
                side_men_[s] = 0;
                side_morale_[s] = 0;
                side_regiments_[s] = 0;
                side_fit_morale_[s] = 0;
                side_fit_[s] = 0;
            }
        });
        for_each_alive(regiments.alive, [&](std::uint32_t r) {
            const std::uint8_t place = regiments.place[r];
            if (place == kPlaceNone) {
                return;
            }
            const std::uint32_t b = state.armies.battle[regiments.army[r]] - 1u;
            const std::size_t s = side_index(b, side_of(state, b, static_cast<std::uint16_t>(r)));
            const std::int32_t men = regiments.strength[r];
            const Fixed morale = regiments.morale[r];
            side_men_[s] += men;
            if (men > 0) {
                side_morale_[s] += morale;
                ++side_regiments_[s];
                if (place != kPlaceOut && morale > 9) {
                    side_fit_morale_[s] += morale;
                    ++side_fit_[s];
                }
            }
        });
    }

    // ------------------------------------------------------------------------------------------------ one battle

    bool Combat::fight(WorldState& state, const Content& content, Movement& movement, std::uint32_t battle,
                       std::uint32_t seed, std::uint32_t day)
    {
        Battles& battles = state.battles;
        const CombatDefines& defines = content.combat;
        ++battles.duration[battle];
        if (battles.phase_day[battle] == kNotDeployed) {
            // The first day (EU4's _nDay = -1): the dice, the lines drawn up, and a side ten times weaker wiped out
            // before a shot is fired.
            roll_dice(state, content, battle, seed, day);
            arrange(state, content, battle, 0);
            arrange(state, content, battle, 1);
            reinforce(state, content, battle, 0);
            reinforce(state, content, battle, 1);
            const int wiped = overrun(state, content, battle, defines.overrun_factor, false);
            if (wiped >= 0) {
                battles.duration[battle] = 0;   // not a shot fired
                finish(state, content, movement, battle, 1 - wiped, true);
                return false;
            }
            battles.phase_day[battle] = 0;
        }
        if (battles.phase_day[battle] >= defines.days_per_phase) {
            roll_dice(state, content, battle, seed, day);
            battles.phase_day[battle] = 0;
            battles.phase[battle] = battles.phase[battle] == kPhaseFire ? kPhaseShock : kPhaseFire;
        }
        reorganize(state, content, battle, 0);
        reorganize(state, content, battle, 1);
        shoot(state, content, battle, 0);
        shoot(state, content, battle, 1);
        return true;
    }

    void Combat::settle(WorldState& state, const Content& content, Movement& movement, std::uint32_t battle)
    {
        Battles& battles = state.battles;
        const CombatDefines& defines = content.combat;
        // The attacker is tested first: when both break the same day, the attacker loses (EU4's WillLose order).
        int loser = -1;
        if (will_lose(state, content, battle, 0)) {
            loser = 0;
        } else if (will_lose(state, content, battle, 1)) {
            loser = 1;
        }
        const bool can_leave = battles.duration[battle] >= defines.cannot_retreat_days;
        if (loser >= 0) {
            // EU4's CombatEnds: the loser retreats, unless the winner is overrun_factor times stronger (or twice as
            // strong while the battle cannot be left yet): then it is wiped out.
            const int winner = 1 - loser;
            const std::int64_t mw = side_men_[side_index(battle, winner)];
            const std::int64_t ml = side_men_[side_index(battle, loser)];
            bool wiped = false;
            if (!can_leave && ml < mw / defines.overrun_factor_cannot_leave) {
                wiped = true;
            } else {
                wiped = !(mw / defines.overrun_factor <= ml);
            }
            finish(state, content, movement, battle, winner, wiped);
            return;
        }
        if (!can_leave) {
            const int wiped = overrun(state, content, battle, defines.overrun_factor_cannot_leave, true);
            if (wiped >= 0) {
                finish(state, content, movement, battle, 1 - wiped, true);
                return;
            }
        }
        ++battles.phase_day[battle];
    }

    void Combat::roll_dice(WorldState& state, const Content& content, std::uint32_t battle, std::uint32_t seed,
                           std::uint32_t day)
    {
        // EU4's RollDice: 0..dice_sides - 1, the attacker first; the battle is named by its slot and generation.
        Battles& battles = state.battles;
        const std::uint32_t entity = battle | std::uint32_t{ battles.generation[battle] } << 16;
        const auto sides = static_cast<std::uint32_t>(content.combat.dice_sides);
        battles.dice_attacker[battle] = static_cast<std::uint8_t>(random_below(seed, day, entity, kDiceAttacker, sides));
        battles.dice_defender[battle] = static_cast<std::uint8_t>(random_below(seed, day, entity, kDiceDefender, sides));
    }

    // EU4's ArrangeBattleField (no artillery line): in the centre infantry, as many as the enemy has regiments able to
    // fight; then all the cavalry on the flanks up to the width; then more infantry up to the enemy's + twice the
    // infantry's maneuver; the rest back to the end of the reserve. If the width is short of cavalry + centre, the
    // centre keeps at most width - 2 × min(cavalry / 2, (width - c') / 2).
    void Combat::arrange(WorldState& state, const Content& content, std::uint32_t battle, int side)
    {
        Regiments& regiments = state.regiments;
        const std::size_t s = side_index(battle, side);
        const int enemy = 1 - side;
        int enemy_able = 0;
        for (int position = 0; position < kFrontSlots; ++position) {
            const std::uint16_t r = front(battle, enemy, position);
            if (r != kEmpty && alive(regiments, r) &&
                category_of(content, regiments, r) != RegimentCategory::artillery) {
                ++enemy_able;
            }
        }
        const std::size_t e = side_index(battle, enemy);
        for (std::uint32_t k = reserve_begin_[e]; k < reserve_end_[e]; ++k) {
            const std::uint16_t r = reserve_regiment_[k];
            if (entry_live(state, k) && alive(regiments, r) &&
                category_of(content, regiments, r) != RegimentCategory::artillery) {
                ++enemy_able;
            }
        }
        int centre = 0;
        int flank = 0;
        for (std::uint32_t k = reserve_begin_[s]; k < reserve_end_[s]; ++k) {
            if (!entry_live(state, k)) {
                continue;
            }
            const std::uint16_t r = reserve_regiment_[k];
            if (!alive(regiments, r)) {
                regiments.place[r] = kPlaceOut;   // never drawn up
                continue;
            }
            switch (category_of(content, regiments, r)) {
            case RegimentCategory::infantry:
                centre_[static_cast<std::size_t>(centre++)] = r;
                break;
            case RegimentCategory::cavalry:
                flank_[static_cast<std::size_t>(flank++)] = r;
                break;
            case RegimentCategory::artillery:
                break;   // stays in the reserve
            }
        }
        const int width = width_[battle];
        int c = std::min(centre, enemy_able);
        int half = flank / 2;
        if (half < 1 && flank > 0) {
            half = 1;
        }
        if (width < flank + c) {
            const int c2 = std::min(c, width / 2);
            half = std::min(half, (width - c2) / 2);
            c = std::min(c, width - 2 * half);
        }
        auto put = [&](std::uint16_t r, int position) {
            front(battle, side, position) = r;
            regiments.place[r] = static_cast<std::uint8_t>(position + 1);
        };
        int at = 0;
        int used_centre = 0;
        int used_flank = 0;
        for (; used_centre < c; ++used_centre) {
            put(centre_[static_cast<std::size_t>(used_centre)], order_at(at++));
        }
        for (; used_flank < flank && at < width; ++used_flank) {
            put(flank_[static_cast<std::size_t>(used_flank)], order_at(at++));
        }
        const int infantry_maneuver = centre > 0 ? maneuver(state, content, centre_[static_cast<std::size_t>(centre - 1)]) : 1;
        const int limit = std::min(width, enemy_able + infantry_maneuver * 2);
        while (at < limit && used_centre < centre) {
            put(centre_[static_cast<std::size_t>(used_centre++)], order_at(at++));
        }
        for (int i = used_centre; i < centre; ++i) {
            push_reserve(state, battle, side, centre_[static_cast<std::size_t>(i)]);
        }
        for (int i = used_flank; i < flank; ++i) {
            push_reserve(state, battle, side, flank_[static_cast<std::size_t>(i)]);
        }
    }

    // EU4's ReinforceFrontLine: every empty position, centre outwards, takes the first fit regiment of the reserve.
    void Combat::reinforce(WorldState& state, const Content& content, std::uint32_t battle, int side)
    {
        Regiments& regiments = state.regiments;
        for (int i = 0; i < kMaxCombatWidth; ++i) {
            const int position = order_at(i);
            if (front(battle, side, position) != kEmpty) {
                continue;
            }
            const int k = first_reserve(state, content, battle, side);
            if (k < 0) {
                return;
            }
            const std::uint16_t r = reserve_regiment_[static_cast<std::size_t>(k)];
            front(battle, side, position) = r;
            regiments.place[r] = static_cast<std::uint8_t>(position + 1);   // its reserve entry is dead now
        }
    }

    // EU4's ShuffleUnitsTowardsMiddle: the left half slides right, the right half left, one pass each.
    void Combat::shuffle_to_middle(WorldState& state, std::uint32_t battle, int side)
    {
        Regiments& regiments = state.regiments;
        auto move = [&](int to, int from) {
            const std::uint16_t r = front(battle, side, from);
            front(battle, side, to) = r;
            front(battle, side, from) = kEmpty;
            regiments.place[r] = static_cast<std::uint8_t>(to + 1);
        };
        for (int i = 1; i < kCenter; ++i) {
            if (front(battle, side, i) == kEmpty && front(battle, side, i - 1) != kEmpty) {
                move(i, i - 1);
            }
        }
        for (int i = kMaxCombatWidth - 2; i >= kCenter; --i) {
            if (front(battle, side, i) == kEmpty && front(battle, side, i + 1) != kEmpty) {
                move(i, i + 1);
            }
        }
    }

    // EU4's ReorganizeTroops.
    void Combat::reorganize(WorldState& state, const Content& content, std::uint32_t battle, int side)
    {
        Regiments& regiments = state.regiments;
        // 1. Regiments with no men or broken morale leave the lines.
        for (int position = 0; position < kFrontSlots; ++position) {
            const std::uint16_t r = front(battle, side, position);
            if (r != kEmpty && (regiments.strength[r] < 1 || regiments.morale[r] < kBroken)) {
                regiments.place[r] = kPlaceOut;
                front(battle, side, position) = kEmpty;
            }
        }
        // 2. The first regiment of the front (outermost first) with no enemy within its maneuver goes to the end of
        //    the reserve: one a day.
        for (int i = kMaxCombatWidth - 1; i >= 0; --i) {
            const int position = order_at(i);
            const std::uint16_t r = front(battle, side, position);
            if (r == kEmpty) {
                continue;
            }
            if (best_target(state, battle, side, position, maneuver(state, content, r)) < 0) {
                front(battle, side, position) = kEmpty;
                push_reserve(state, battle, side, r);
                break;
            }
        }
        // 3. The front filled from the reserve; 4. the gaps closed.
        reinforce(state, content, battle, side);
        shuffle_to_middle(state, battle, side);
    }

    // EU4's CLandCombatant::DailyUpdate: the front within the width shoots, centre outwards; the regiments past the
    // width go back to the reserve; the day's morale loss (1% of the enemy's maximum morale in the line, 2% in the
    // reserve); reserve regiments that can no longer fight leave it.
    void Combat::shoot(WorldState& state, const Content& content, std::uint32_t battle, int side)
    {
        Regiments& regiments = state.regiments;
        const CombatDefines& defines = content.combat;
        const int width = width_[battle];
        for (int i = 0; i < width; ++i) {
            const int position = order_at(i);
            const std::uint16_t r = front(battle, side, position);
            if (r != kEmpty) {
                fire_at(state, content, battle, side, position, r);
            }
        }
        for (int i = width; i < kMaxCombatWidth; ++i) {
            const int position = order_at(i);
            const std::uint16_t r = front(battle, side, position);
            if (r != kEmpty) {
                front(battle, side, position) = kEmpty;
                push_reserve(state, battle, side, r);
            }
        }
        const Fixed enemy_max = side_max_morale_[side_index(battle, 1 - side)];
        const Fixed line_loss = fx_mul(enemy_max, defines.daily_morale_damage);
        const Fixed reserve_loss = fx_mul(kFixedOne, fx_mul(enemy_max, defines.reserve_daily_morale_damage));
        for (int position = 0; position < kFrontSlots; ++position) {
            const std::uint16_t r = front(battle, side, position);
            if (r != kEmpty) {
                damage_morale_[r] += line_loss;
            }
        }
        const std::size_t s = side_index(battle, side);
        for (std::uint32_t k = reserve_begin_[s]; k < reserve_end_[s]; ++k) {
            if (!entry_live(state, k)) {
                continue;
            }
            const std::uint16_t r = reserve_regiment_[k];
            damage_morale_[r] += reserve_loss;
            if (regiments.strength[r] < 1 || regiments.morale[r] < kBroken) {
                regiments.place[r] = kPlaceOut;   // unable before today's damage
            }
        }
    }

    // EU4's CLandCombatant::FireAt, the operations in the game's order (BattleSim's fireAt); the factors that are 1
    // here (combat ability, the damage dealt and received, the morale damage) are left out, as multiplying by 1.000
    // changes nothing.
    void Combat::fire_at(const WorldState& state, const Content& content, std::uint32_t battle, int side, int position,
                         std::uint16_t regiment)
    {
        const Battles& battles = state.battles;
        const Regiments& regiments = state.regiments;
        const RegimentTypeNumbers& n = content.regiment_numbers;
        const CombatDefines& defines = content.combat;
        const int enemy = 1 - side;
        const int target_position = best_target(state, battle, side, position, maneuver(state, content, regiment));
        if (target_position < 0) {
            return;
        }
        const std::uint16_t target = front(battle, enemy, target_position);
        const bool flank = front(battle, enemy, position) == kEmpty;   // nobody straight ahead: a flank attack

        const std::uint8_t phase = battles.phase[battle];
        const std::uint8_t mine = regiments.type[regiment];
        const std::uint8_t theirs = regiments.type[target];
        const int offence = phase == kPhaseFire ? n.off_fire[mine] : n.off_shock[mine];
        const int defence = phase == kPhaseFire ? n.def_fire[theirs] : n.def_shock[theirs];
        const int phase_pips = offence - defence;
        const int morale_pips = (n.off_morale[mine] - n.def_morale[theirs]) + phase_pips;
        const int dice = side == 0 ? battles.dice_attacker[battle] : battles.dice_defender[battle];
        constexpr int kGlobalModifier = 0;   // generals, terrain, crossings: not yet
        const int x = std::clamp(phase_pips + kGlobalModifier + dice + 2, 0, 21);
        const int xm = std::clamp(morale_pips + kGlobalModifier + dice + 2, 0, 21);
        const Fixed base = 25 * x + 25;   // × 0.2 below: 15 + 5 × (dice + pips + 2)
        const Fixed base_morale = 25 * xm + 25;

        const std::size_t s = side_index(battle, side);
        const Fixed tactics = side_tactics_[side_index(battle, enemy)];
        const RegimentCategory category = category_of(content, regiments, regiment);
        const int weapon_index = (category == RegimentCategory::cavalry ? 2 : 0) + (phase == kPhaseFire ? 0 : 1);
        const Fixed weapon = category == RegimentCategory::artillery ? 0 : side_weapon_[s * 4 + weapon_index];
        const Fixed discipline = side_discipline_[s];
        const std::int32_t strength = regiments.strength[regiment];
        const Fixed range = defines.front_line_modifier;
        const Fixed length = kFixedOne + battles.duration[battle] * defines.war_length_damage;

        // Casualties.
        Fixed men = fx_mul(strength, base);
        men = fx_mul(discipline, men);
        men = fx_mul(range, men);
        if (tactics > 0) {
            men = fx_div(men, tactics);
        }
        men = fx_mul(weapon, men);
        men = static_cast<Fixed>(std::int64_t{ men } * 200 / 1000);
        if (flank) {
            men = fx_mul(ranks(strength) * 250, men);
        }
        men = fx_mul(men, length);

        // Morale.
        Fixed morale = fx_div(side_max_morale_[s], 2700);   // the shooter's country's maximum morale / 2.7
        morale = fx_mul(morale, base_morale);
        morale = fx_mul(strength, morale);
        morale = fx_mul(discipline, morale);
        morale = fx_mul(range, morale);
        if (tactics > 0) {
            morale = fx_div(morale, tactics);
        }
        morale = fx_mul(morale, weapon);
        morale = fx_mul(morale, length);

        damage_men_[target] += std::max(men, 0);
        damage_morale_[target] += std::max(morale, 0);
    }

    // ------------------------------------------------------------------------------------------------ queries

    // EU4's CSubUnit::GetManeuverValue: the unit's maneuver × (1 + technology's, 0 yet), × 100/50/25/0% by its ranks,
    // whole, at least 1.
    int Combat::maneuver(const WorldState& state, const Content& content, std::uint16_t regiment) const
    {
        const std::int64_t base = std::int64_t{ content.regiment_numbers.maneuver[state.regiments.type[regiment]] } *
                                  kFixedOne;
        const int rank = ranks(state.regiments.strength[regiment]);
        const std::int64_t factor = rank == 4 ? 1000 : rank == 3 ? 500 : rank == 2 ? 250 : 0;
        return std::max(1, static_cast<int>(base * factor / 1000 / 1000));
    }

    // EU4's GetBestEnemyTarget: the regiment straight ahead if it can fight; else the first able one from left to
    // right within ±range; else, only if the position straight ahead is empty, the first one there at all in range;
    // -1: nothing in range.
    int Combat::best_target(const WorldState& state, std::uint32_t battle, int side, int position, int range) const
    {
        const Regiments& regiments = state.regiments;
        const int enemy = 1 - side;
        const std::uint16_t opposite = front(battle, enemy, position);
        if (opposite != kEmpty && alive(regiments, opposite)) {
            return position;
        }
        const int lo = std::max(position - range, 0);
        const int hi = std::min(position + range, kMaxCombatWidth);
        for (int i = lo; i <= hi; ++i) {
            const std::uint16_t r = front(battle, enemy, i);
            if (r != kEmpty && alive(regiments, r)) {
                return i;
            }
        }
        if (opposite == kEmpty) {
            for (int i = lo; i <= hi; ++i) {
                if (front(battle, enemy, i) != kEmpty) {
                    return i;
                }
            }
            return -1;
        }
        return position;   // a broken regiment straight ahead
    }

    // EU4's GetBestReserveUnit(FRONT): the first in queue order with men and morale, not artillery.
    int Combat::first_reserve(const WorldState& state, const Content& content, std::uint32_t battle, int side) const
    {
        const std::size_t s = side_index(battle, side);
        for (std::uint32_t k = reserve_begin_[s]; k < reserve_end_[s]; ++k) {
            const std::uint16_t r = reserve_regiment_[k];
            if (entry_live(state, k) && alive(state.regiments, r) &&
                category_of(content, state.regiments, r) != RegimentCategory::artillery) {
                return static_cast<int>(k);
            }
        }
        return -1;
    }

    // EU4's HasAnyTroopsAbleToFight: anyone in the line (even broken, until the next reorganisation), or a fit
    // regiment in the reserve.
    bool Combat::able_to_fight(const WorldState& state, const Content& content, std::uint32_t battle, int side) const
    {
        for (int position = 0; position < kFrontSlots; ++position) {
            if (front(battle, side, position) != kEmpty) {
                return true;
            }
        }
        return first_reserve(state, content, battle, side) >= 0;
    }

    // EU4's HasMoraleCollapsed: the average morale of the regiments in the lines and the reserve with men and morale
    // (> 0.009) is not above 0.010, or there are none.
    bool Combat::morale_collapsed(std::uint32_t battle, int side) const
    {
        const std::size_t s = side_index(battle, side);
        if (side_fit_[s] == 0) {
            return true;
        }
        return !(10 < side_fit_morale_[s] / side_fit_[s]);
    }

    // EU4's CanMoraleCollapse: the average morale of every regiment of the side (the broken ones included) below
    // 0.25, or nobody able to fight.
    bool Combat::can_morale_collapse(const WorldState& state, const Content& content, std::uint32_t battle,
                                     int side) const
    {
        const std::size_t s = side_index(battle, side);
        if (side_regiments_[s] == 0) {
            return true;
        }
        return !(0xF9 < side_morale_[s] / side_regiments_[s] && able_to_fight(state, content, battle, side));
    }

    // EU4's WillLose: under 10 men; or, once the battle can be left, its morale collapsed.
    bool Combat::will_lose(const WorldState& state, const Content& content, std::uint32_t battle, int side) const
    {
        if (side_men_[side_index(battle, side)] < 10) {
            return true;
        }
        if (state.battles.duration[battle] < content.combat.cannot_retreat_days) {
            return false;
        }
        return morale_collapsed(battle, side);
    }

    // EU4's CheckOverrun.
    int Combat::overrun(const WorldState& state, const Content& content, std::uint32_t battle, int factor,
                        bool daily) const
    {
        const int width = width_[battle];
        std::int64_t men_a = side_men_[side_index(battle, 0)];
        std::int64_t men_b = side_men_[side_index(battle, 1)];
        if (!daily && men_a / 1000 >= width && men_b / 1000 >= width) {
            return -1;   // both fill the width
        }
        const bool first_day = state.battles.phase_day[battle] == kNotDeployed;
        const bool able_a = able_to_fight(state, content, battle, 0);
        const bool able_b = able_to_fight(state, content, battle, 1);
        // A side nobody of which can fight counts as 0 men if it has less than half the enemy's, or on the first day.
        if (!able_b && (men_b < men_a / 2 || first_day)) {
            men_b = 0;
        }
        if (!able_a && (men_a < men_b / 2 || first_day)) {
            men_a = 0;
        }
        bool check_a = true;
        if (!daily || can_morale_collapse(state, content, battle, 1)) {
            if (able_a && men_b < men_a / factor) {
                return 1;
            }
            check_a = !daily || can_morale_collapse(state, content, battle, 0);
        } else {
            check_a = can_morale_collapse(state, content, battle, 0);
        }
        if (check_a && able_b && men_a < men_b / factor) {
            return 0;
        }
        return -1;
    }

    // ------------------------------------------------------------------------------------------------ the reserve

    void Combat::push_reserve(WorldState& state, std::uint32_t battle, int side, std::uint16_t regiment)
    {
        const std::size_t s = side_index(battle, side);
        if (reserve_end_[s] == reserve_begin_[s] + reserve_room_[s]) {
            compact_reserve(state, s);
        }
        if (reserve_end_[s] == reserve_begin_[s] + reserve_room_[s]) {
            log::error("battle {}: the reserve queue of side {} is full", battle, side);
            state.regiments.place[regiment] = kPlaceOut;
            return;
        }
        const std::uint32_t number = state.battles.queue[battle]++;
        state.regiments.place[regiment] = kPlaceReserve;
        state.regiments.queue[regiment] = number;
        reserve_regiment_[reserve_end_[s]] = regiment;
        reserve_queue_[reserve_end_[s]] = number;
        ++reserve_end_[s];
    }

    bool Combat::entry_live(const WorldState& state, std::uint32_t k) const
    {
        const std::uint16_t r = reserve_regiment_[k];
        return slot_alive(state.regiments.alive, r) && state.regiments.place[r] == kPlaceReserve &&
               state.regiments.queue[r] == reserve_queue_[k];
    }

    void Combat::compact_reserve(const WorldState& state, std::size_t s)
    {
        std::uint32_t to = reserve_begin_[s];
        for (std::uint32_t k = reserve_begin_[s]; k < reserve_end_[s]; ++k) {
            if (entry_live(state, k)) {
                reserve_regiment_[to] = reserve_regiment_[k];
                reserve_queue_[to] = reserve_queue_[k];
                ++to;
            }
        }
        reserve_end_[s] = to;
    }

    // ------------------------------------------------------------------------------------------------ the end

    void Combat::finish(WorldState& state, const Content& content, Movement& movement, std::uint32_t battle,
                        int winner, bool wiped)
    {
        Armies& armies = state.armies;
        Regiments& regiments = state.regiments;
        Battles& battles = state.battles;
        const int loser = 1 - winner;
        const std::uint16_t province = battles.province[battle];
        const std::uint16_t countries[2] = { battles.attacker[battle], battles.defender[battle] };
        const std::int64_t joined[2] = { battles.joined_attacker[battle], battles.joined_defender[battle] };

        // EU4's GiveEndOfCombatMoraleBoost: every regiment of the winner has at least its maximum morale ×
        // min(0.75, 1.25 × the loser's size / the winner's).
        const std::int64_t share = std::min<std::int64_t>(750, 1250 * joined[loser] / std::max<std::int64_t>(1, joined[winner]));
        const Fixed floor = fx_mul(side_max_morale_[side_index(battle, winner)], share);

        std::uint16_t destination = 0;
        bool searched = false;
        for (std::uint32_t k = army_offsets_[province]; k < army_offsets_[province + 1]; ++k) {
            const std::uint16_t a = army_list_[k];
            if (!slot_alive(armies.alive, a) || armies.battle[a] != battle + 1) {
                continue;
            }
            const int side = armies.owner[a] == countries[0] ? 0 : 1;
            for (std::uint32_t j = regiment_offsets_[a]; j < regiment_offsets_[a + 1]; ++j) {
                const std::uint16_t r = regiment_list_[j];
                if (!slot_alive(regiments.alive, r) || regiments.army[r] != a) {
                    continue;
                }
                if (side == winner && regiments.strength[r] > 0) {
                    regiments.morale[r] = std::max(regiments.morale[r], floor);
                }
                regiments.place[r] = kPlaceNone;
                regiments.queue[r] = 0;
            }
            armies.battle[a] = 0;
            if (side != loser) {
                continue;
            }
            if (wiped) {
                destroy_army(state, a);
                continue;
            }
            // The loser retreats (docs/GAMEDATA.md, "Бой"): its march is locked until it arrives.
            if (!searched) {
                destination = retreat_to(state, movement, countries[loser], province);
                searched = true;
            }
            if (destination != 0 && movement.order(state, a, destination)) {
                set_retreating(armies, a, true);
            }
        }
        log::info("battle in province {} after {} days: {} beats {}, who {}", province, battles.duration[battle],
                  content.countries.tag_text(countries[winner]), content.countries.tag_text(countries[loser]),
                  wiped ? "is wiped out" : destination != 0 ? "retreats" : "has nowhere to retreat");

        std::fill_n(front_.begin() + static_cast<std::ptrdiff_t>(side_index(battle, 0) * kFrontSlots),
                    2 * kFrontSlots, kEmpty);
        province_battle_[province] = 0;
        const std::uint16_t generation = battles.generation[battle];
#define MC_CLEAR_BATTLE(table, type, name, start) battles.name[battle] = type{};
        MC_BATTLE_COLUMNS(MC_CLEAR_BATTLE)
#undef MC_CLEAR_BATTLE
        battles.generation[battle] = static_cast<std::uint16_t>(generation + 1);
        battles.alive[battle / 64] &= ~(std::uint64_t{ 1 } << (battle % 64));
    }

    // Where a beaten country's armies retreat from `from` (decision of the user, 26.11.2026): its own province the
    // farthest away by the march; with none, the nearest province without foreign armies. Ties: the lower id. 0 if
    // there is no way anywhere.
    std::uint16_t Combat::retreat_to(const WorldState& state, Movement& movement, std::uint16_t country,
                                     std::uint16_t from) const
    {
        const Armies& armies = state.armies;
        movement.search_from(from);
        std::uint16_t best = 0;
        float best_way = -1.0f;
        for (std::uint32_t p = 1; p < province_count_; ++p) {
            const float way = movement.way_from(static_cast<std::uint16_t>(p));
            if (p != from && state.provinces.owner[p] == country && way > best_way) {
                best = static_cast<std::uint16_t>(p);
                best_way = way;
            }
        }
        if (best != 0) {
            return best;
        }
        float nearest = std::numeric_limits<float>::max();
        for (std::uint32_t p = 1; p < province_count_; ++p) {
            const float way = movement.way_from(static_cast<std::uint16_t>(p));
            if (p == from || way < 0.0f || !(way < nearest)) {
                continue;
            }
            bool enemies = false;
            for (std::uint32_t k = army_offsets_[p]; k < army_offsets_[p + 1] && !enemies; ++k) {
                const std::uint16_t a = army_list_[k];
                enemies = slot_alive(armies.alive, a) && armies.owner[a] != 0 && armies.owner[a] != country;
            }
            if (!enemies) {
                best = static_cast<std::uint16_t>(p);
                nearest = way;
            }
        }
        return best;
    }
}
