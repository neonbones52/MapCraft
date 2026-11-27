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

        // A battle names a regiment by its slot + 1, 0 = none (game/game_data.h, Battle): the slot, -1 for none.
        int regiment_of(std::uint16_t ref) { return static_cast<int>(ref) - 1; }
        std::uint16_t ref_of(std::uint16_t regiment) { return static_cast<std::uint16_t>(regiment + 1); }

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

        // ---------------------------------------------------------------------------------------------- the reserve

        // To the end of the side's reserve (EU4's _Reserves.push_back).
        void reserve_push(Regiments& regiments, BattleSide& side, std::uint16_t r)
        {
            regiments.place[r] = kPlaceReserve;
            regiments.reserve_prev[r] = side.reserve_last;
            regiments.reserve_next[r] = 0;
            if (side.reserve_last != 0) {
                regiments.reserve_next[regiment_of(side.reserve_last)] = ref_of(r);
            } else {
                side.reserve_first = ref_of(r);
            }
            side.reserve_last = ref_of(r);
        }

        // Out of the side's reserve; its place is the caller's to set.
        void reserve_remove(Regiments& regiments, BattleSide& side, std::uint16_t r)
        {
            const std::uint16_t prev = regiments.reserve_prev[r];
            const std::uint16_t next = regiments.reserve_next[r];
            (prev != 0 ? regiments.reserve_next[regiment_of(prev)] : side.reserve_first) = next;
            (next != 0 ? regiments.reserve_prev[regiment_of(next)] : side.reserve_last) = prev;
            regiments.reserve_prev[r] = 0;
            regiments.reserve_next[r] = 0;
        }

        // f(regiment) over the side's reserve in its order; f may take the regiment out or push it to the end (it is
        // then met again).
        template <class F>
        void for_each_reserve(const Regiments& regiments, const BattleSide& side, F&& f)
        {
            for (std::uint16_t ref = side.reserve_first; ref != 0;) {
                const auto r = static_cast<std::uint16_t>(regiment_of(ref));
                ref = regiments.reserve_next[r];
                f(r);
            }
        }

        // ---------------------------------------------------------------------------------------------- queries

        // EU4's CSubUnit::GetManeuverValue: the unit's maneuver × (1 + technology's, 0 yet), × 100/50/25/0% by its
        // ranks, whole, at least 1.
        int maneuver(const WorldState& state, const Content& content, std::uint16_t regiment)
        {
            const std::int64_t base =
                std::int64_t{ content.regiment_numbers.maneuver[state.regiments.type[regiment]] } * kFixedOne;
            const int rank = ranks(state.regiments.strength[regiment]);
            const std::int64_t factor = rank == 4 ? 1000 : rank == 3 ? 500 : rank == 2 ? 250 : 0;
            return std::max(1, static_cast<int>(base * factor / 1000 / 1000));
        }

        // EU4's GetBestEnemyTarget: the regiment straight ahead if it can fight; else the first able one from left to
        // right within ±range; else, only if the position straight ahead is empty, the first one there at all in
        // range; -1: nothing in range.
        int best_target(const Regiments& regiments, const BattleSide& enemy, int position, int range)
        {
            const int opposite = regiment_of(enemy.front[static_cast<std::size_t>(position)]);
            if (opposite >= 0 && alive(regiments, static_cast<std::uint16_t>(opposite))) {
                return position;
            }
            const int lo = std::max(position - range, 0);
            const int hi = std::min(position + range, kMaxCombatWidth);
            for (int i = lo; i <= hi; ++i) {
                const int r = regiment_of(enemy.front[static_cast<std::size_t>(i)]);
                if (r >= 0 && alive(regiments, static_cast<std::uint16_t>(r))) {
                    return i;
                }
            }
            if (opposite < 0) {
                for (int i = lo; i <= hi; ++i) {
                    if (enemy.front[static_cast<std::size_t>(i)] != 0) {
                        return i;
                    }
                }
                return -1;
            }
            return position;   // a broken regiment straight ahead
        }

        // EU4's GetBestReserveUnit(FRONT): the first in the reserve's order with men and morale, not artillery; -1
        // if none.
        int first_reserve(const Regiments& regiments, const Content& content, const BattleSide& side)
        {
            for (std::uint16_t ref = side.reserve_first; ref != 0;) {
                const auto r = static_cast<std::uint16_t>(regiment_of(ref));
                if (alive(regiments, r) && category_of(content, regiments, r) != RegimentCategory::artillery) {
                    return r;
                }
                ref = regiments.reserve_next[r];
            }
            return -1;
        }

        // EU4's HasAnyTroopsAbleToFight: anyone in the line (even broken, until the next reorganisation), or a fit
        // regiment in the reserve.
        bool able_to_fight(const Regiments& regiments, const Content& content, const BattleSide& side)
        {
            for (const std::uint16_t ref : side.front) {
                if (ref != 0) {
                    return true;
                }
            }
            return first_reserve(regiments, content, side) >= 0;
        }

        // ---------------------------------------------------------------------------------------------- the lines

        void put(Regiments& regiments, BattleSide& side, std::uint16_t r, int position)
        {
            side.front[static_cast<std::size_t>(position)] = ref_of(r);
            regiments.place[r] = static_cast<std::uint8_t>(position + 1);
        }

        // From the front to the end of the reserve.
        void to_reserve(Regiments& regiments, BattleSide& side, int position)
        {
            const auto r = static_cast<std::uint16_t>(regiment_of(side.front[static_cast<std::size_t>(position)]));
            side.front[static_cast<std::size_t>(position)] = 0;
            reserve_push(regiments, side, r);
        }

        // EU4's ReinforceFrontLine: every empty position, centre outwards, takes the first fit regiment of the reserve.
        void reinforce(Regiments& regiments, const Content& content, BattleSide& side)
        {
            for (int i = 0; i < kMaxCombatWidth; ++i) {
                const int position = order_at(i);
                if (side.front[static_cast<std::size_t>(position)] != 0) {
                    continue;
                }
                const int r = first_reserve(regiments, content, side);
                if (r < 0) {
                    return;
                }
                reserve_remove(regiments, side, static_cast<std::uint16_t>(r));
                put(regiments, side, static_cast<std::uint16_t>(r), position);
            }
        }

        // EU4's ShuffleUnitsTowardsMiddle: the left half slides right, the right half left, one pass each.
        void shuffle_to_middle(Regiments& regiments, BattleSide& side)
        {
            auto move = [&](int to, int from) {
                const auto r = static_cast<std::uint16_t>(regiment_of(side.front[static_cast<std::size_t>(from)]));
                side.front[static_cast<std::size_t>(from)] = 0;
                put(regiments, side, r, to);
            };
            for (int i = 1; i < kCenter; ++i) {
                if (side.front[static_cast<std::size_t>(i)] == 0 && side.front[static_cast<std::size_t>(i - 1)] != 0) {
                    move(i, i - 1);
                }
            }
            for (int i = kMaxCombatWidth - 2; i >= kCenter; --i) {
                if (side.front[static_cast<std::size_t>(i)] == 0 && side.front[static_cast<std::size_t>(i + 1)] != 0) {
                    move(i, i + 1);
                }
            }
        }

        // EU4's ReorganizeTroops.
        void reorganize(WorldState& state, const Content& content, BattleSide& side, const BattleSide& enemy)
        {
            Regiments& regiments = state.regiments;
            // 1. Regiments with no men or broken morale leave the lines.
            for (std::uint16_t& ref : side.front) {
                const int r = regiment_of(ref);
                if (r >= 0 && (regiments.strength[r] < 1 || regiments.morale[r] < kBroken)) {
                    regiments.place[r] = kPlaceOut;
                    ref = 0;
                }
            }
            // 2. The first regiment of the front (outermost first) with no enemy within its maneuver goes to the end
            //    of the reserve: one a day.
            for (int i = kMaxCombatWidth - 1; i >= 0; --i) {
                const int position = order_at(i);
                const int r = regiment_of(side.front[static_cast<std::size_t>(position)]);
                if (r < 0) {
                    continue;
                }
                if (best_target(regiments, enemy, position, maneuver(state, content, static_cast<std::uint16_t>(r))) <
                    0) {
                    to_reserve(regiments, side, position);
                    break;
                }
            }
            // 3. The front filled from the reserve; 4. the gaps closed.
            reinforce(regiments, content, side);
            shuffle_to_middle(regiments, side);
        }
    }

    void Combat::build(const MapData& map)
    {
        province_count_ = map.count;
        army_offsets_.assign(padded(std::size_t{ province_count_ } + 1), 0);
        army_list_.assign(kArmySlots, 0);
        fill_.assign(padded(std::size_t{ province_count_ } + 1), 0);
        army_regiments_.assign(kArmySlots, 0);
        province_battle_.assign(padded(province_count_), 0);
        damage_men_.assign(kRegimentSlots, 0);
        damage_morale_.assign(kRegimentSlots, 0);
        today_.assign(kBattleSlots, BattleDay{});
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
        prepare(state, content, sums);
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
            const std::uint8_t place = regiments.place[r];
            if (place == kPlaceNone || regiments.strength[r] != 0) {
                return;
            }
            const std::uint16_t army = regiments.army[r];
            BattleSide& side = state.battles.list[armies.battle[army] - 1u].sides[armies.side[army]];
            if (in_front(place)) {
                side.front[place - 1u] = 0;
            } else if (place == kPlaceReserve) {
                reserve_remove(regiments, side, static_cast<std::uint16_t>(r));
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
        std::fill(army_regiments_.begin(), army_regiments_.end(), 0u);
        for_each_alive(regiments.alive, [&](std::uint32_t r) { ++army_regiments_[regiments.army[r]]; });
    }

    void Combat::meet(WorldState& state, const Content& content, Movement& movement, std::uint32_t day)
    {
        Armies& armies = state.armies;
        Battles& battles = state.battles;
        std::fill(province_battle_.begin(), province_battle_.end(), std::uint16_t{ 0 });
        for_each_alive(battles.alive, [&](std::uint32_t b) {
            province_battle_[battles.list[b].province] = static_cast<std::uint16_t>(b + 1);
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
                // The slot is Battle{} but for its generation.
                Battle& battle = battles.list[b];
                battle.province = static_cast<std::uint16_t>(p);
                battle.sides[0].country = attacker;
                battle.sides[1].country = defender;
                battle.phase_day = kNotDeployed;
                province_battle_[p] = static_cast<std::uint16_t>(b + 1);
                log::info("day {}: battle in province {}: {} attacks {}", day, p, content.countries.tag_text(attacker),
                          content.countries.tag_text(defender));
            }
            // The armies of the battle's two countries that are not in it yet join it (EU4's TryToJoin), in slot
            // order.
            const std::uint32_t b = province_battle_[p] - 1u;
            const Battle& battle = battles.list[b];
            for (std::uint32_t k = lo; k < hi; ++k) {
                const std::uint16_t a = army_list_[k];
                const std::uint16_t owner = armies.owner[a];
                if (fighting(a) && armies.battle[a] == 0 &&
                    (owner == battle.sides[0].country || owner == battle.sides[1].country)) {
                    join(state, movement, b, a);
                }
            }
        }
    }

    void Combat::join(WorldState& state, Movement& movement, std::uint32_t battle, std::uint16_t army)
    {
        Battle& b = state.battles.list[battle];
        Regiments& regiments = state.regiments;
        const int side = state.armies.owner[army] == b.sides[0].country ? 0 : 1;
        state.armies.battle[army] = static_cast<std::uint16_t>(battle + 1);
        state.armies.side[army] = static_cast<std::uint8_t>(side);
        movement.halt(state, army);
        // Its regiments go to the end of its side's reserve, in slot order (EU4's AddUnit), with the morale they have.
        for_each_alive(regiments.alive, [&](std::uint32_t r) {
            if (regiments.army[r] == army) {
                reserve_push(regiments, b.sides[side], static_cast<std::uint16_t>(r));
                b.joined[side] += regiments.strength[r];
            }
        });
    }

    void Combat::prepare(const WorldState& state, const Content& content, const CountryModifiers& sums)
    {
        for_each_alive(state.battles.alive, [&](std::uint32_t b) {
            const Battle& battle = state.battles.list[b];
            BattleDay& today = today_[b];
            for (int s = 0; s < 2; ++s) {
                SideDay& side = today.sides[s];
                const std::uint16_t c = battle.sides[s].country;
                side.discipline = discipline(sums, c);
                side.tactics = tactics(sums, c);
                side.max_morale = max_morale(sums, c);
                side.weapon[0] = weapon(sums, c, RegimentCategory::infantry, kPhaseFire);
                side.weapon[1] = weapon(sums, c, RegimentCategory::infantry, kPhaseShock);
                side.weapon[2] = weapon(sums, c, RegimentCategory::cavalry, kPhaseFire);
                side.weapon[3] = weapon(sums, c, RegimentCategory::cavalry, kPhaseShock);
            }
            // The width: the wider country's, within the defines' limit.
            const int width = std::max(combat_width(sums, battle.sides[0].country),
                                       combat_width(sums, battle.sides[1].country));
            today.width = std::clamp(width, 1, content.combat.max_combat_width);
        });
    }

    void Combat::tally(const WorldState& state)
    {
        const Armies& armies = state.armies;
        const Regiments& regiments = state.regiments;
        for_each_alive(state.battles.alive, [&](std::uint32_t b) {
            for (SideDay& side : today_[b].sides) {
                side.men = 0;
                side.morale = 0;
                side.regiments = 0;
                side.fit_morale = 0;
                side.fit = 0;
            }
        });
        for_each_alive(regiments.alive, [&](std::uint32_t r) {
            const std::uint8_t place = regiments.place[r];
            if (place == kPlaceNone) {
                return;
            }
            const std::uint16_t army = regiments.army[r];
            SideDay& side = today_[armies.battle[army] - 1u].sides[armies.side[army]];
            const std::int32_t men = regiments.strength[r];
            const Fixed morale = regiments.morale[r];
            side.men += men;
            if (men > 0) {
                side.morale += morale;
                ++side.regiments;
                if (place != kPlaceOut && morale > 9) {
                    side.fit_morale += morale;
                    ++side.fit;
                }
            }
        });
    }

    // ------------------------------------------------------------------------------------------------ one battle

    bool Combat::fight(WorldState& state, const Content& content, Movement& movement, std::uint32_t battle,
                       std::uint32_t seed, std::uint32_t day)
    {
        Battle& b = state.battles.list[battle];
        const CombatDefines& defines = content.combat;
        // EU4's RollDice: 0..dice_sides - 1, the attacker first; the battle is named by its slot and generation.
        auto roll_dice = [&] {
            const std::uint32_t entity = battle | std::uint32_t{ b.generation } << 16;
            const auto sides = static_cast<std::uint32_t>(defines.dice_sides);
            b.sides[0].dice = static_cast<std::uint16_t>(random_below(seed, day, entity, kDiceAttacker, sides));
            b.sides[1].dice = static_cast<std::uint16_t>(random_below(seed, day, entity, kDiceDefender, sides));
        };
        ++b.duration;
        if (b.phase_day == kNotDeployed) {
            // The first day (EU4's _nDay = -1): the dice, the lines drawn up, and a side ten times weaker wiped out
            // before a shot is fired.
            roll_dice();
            arrange(state, content, battle, 0);
            arrange(state, content, battle, 1);
            reinforce(state.regiments, content, b.sides[0]);
            reinforce(state.regiments, content, b.sides[1]);
            const int wiped = overrun(state, content, battle, defines.overrun_factor, false);
            if (wiped >= 0) {
                b.duration = 0;   // not a shot fired
                finish(state, content, movement, battle, 1 - wiped, true);
                return false;
            }
            b.phase_day = 0;
        }
        if (b.phase_day >= defines.days_per_phase) {
            roll_dice();
            b.phase_day = 0;
            b.phase = b.phase == kPhaseFire ? kPhaseShock : kPhaseFire;
        }
        reorganize(state, content, b.sides[0], b.sides[1]);
        reorganize(state, content, b.sides[1], b.sides[0]);
        shoot(state, content, battle, 0);
        shoot(state, content, battle, 1);
        return true;
    }

    void Combat::settle(WorldState& state, const Content& content, Movement& movement, std::uint32_t battle)
    {
        Battle& b = state.battles.list[battle];
        const CombatDefines& defines = content.combat;
        // The attacker is tested first: when both break the same day, the attacker loses (EU4's WillLose order).
        int loser = -1;
        if (will_lose(state, content, battle, 0)) {
            loser = 0;
        } else if (will_lose(state, content, battle, 1)) {
            loser = 1;
        }
        const bool can_leave = b.duration >= defines.cannot_retreat_days;
        if (loser >= 0) {
            // EU4's CombatEnds: the loser retreats, unless the winner is overrun_factor times stronger (or twice as
            // strong while the battle cannot be left yet): then it is wiped out.
            const int winner = 1 - loser;
            const std::int64_t mw = today_[battle].sides[winner].men;
            const std::int64_t ml = today_[battle].sides[loser].men;
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
        ++b.phase_day;
    }

    // EU4's ArrangeBattleField (no artillery line): in the centre infantry, as many as the enemy has regiments able to
    // fight; then all the cavalry on the flanks up to the width; then more infantry up to the enemy's + twice the
    // infantry's maneuver; the rest back to the end of the reserve. If the width is short of cavalry + centre, the
    // centre keeps at most width - 2 × min(cavalry / 2, (width - c') / 2).
    void Combat::arrange(WorldState& state, const Content& content, std::uint32_t battle, int side)
    {
        Regiments& regiments = state.regiments;
        BattleSide& mine = state.battles.list[battle].sides[side];
        const BattleSide& enemy = state.battles.list[battle].sides[1 - side];
        auto able = [&](std::uint16_t r) {
            return alive(regiments, r) && category_of(content, regiments, r) != RegimentCategory::artillery;
        };
        int enemy_able = 0;
        for (const std::uint16_t ref : enemy.front) {
            if (ref != 0 && able(static_cast<std::uint16_t>(regiment_of(ref)))) {
                ++enemy_able;
            }
        }
        for_each_reserve(regiments, enemy, [&](std::uint16_t r) {
            if (able(r)) {
                ++enemy_able;
            }
        });
        // The reserve's infantry and cavalry taken out in its order, the broken out of the lines; artillery stays.
        int centre = 0;
        int flank = 0;
        for_each_reserve(regiments, mine, [&](std::uint16_t r) {
            if (!alive(regiments, r)) {
                reserve_remove(regiments, mine, r);
                regiments.place[r] = kPlaceOut;   // never drawn up
                return;
            }
            switch (category_of(content, regiments, r)) {
            case RegimentCategory::infantry:
                reserve_remove(regiments, mine, r);
                centre_[static_cast<std::size_t>(centre++)] = r;
                break;
            case RegimentCategory::cavalry:
                reserve_remove(regiments, mine, r);
                flank_[static_cast<std::size_t>(flank++)] = r;
                break;
            case RegimentCategory::artillery:
                break;
            }
        });
        const int width = today_[battle].width;
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
        int at = 0;
        int used_centre = 0;
        int used_flank = 0;
        for (; used_centre < c; ++used_centre) {
            put(regiments, mine, centre_[static_cast<std::size_t>(used_centre)], order_at(at++));
        }
        for (; used_flank < flank && at < width; ++used_flank) {
            put(regiments, mine, flank_[static_cast<std::size_t>(used_flank)], order_at(at++));
        }
        const int infantry_maneuver =
            centre > 0 ? maneuver(state, content, centre_[static_cast<std::size_t>(centre - 1)]) : 1;
        const int limit = std::min(width, enemy_able + infantry_maneuver * 2);
        while (at < limit && used_centre < centre) {
            put(regiments, mine, centre_[static_cast<std::size_t>(used_centre++)], order_at(at++));
        }
        for (int i = used_centre; i < centre; ++i) {
            reserve_push(regiments, mine, centre_[static_cast<std::size_t>(i)]);
        }
        for (int i = used_flank; i < flank; ++i) {
            reserve_push(regiments, mine, flank_[static_cast<std::size_t>(i)]);
        }
    }

    // EU4's CLandCombatant::DailyUpdate: the front within the width shoots, centre outwards; the regiments past the
    // width go back to the reserve; the day's morale loss (1% of the enemy's maximum morale in the line, 2% in the
    // reserve); reserve regiments that can no longer fight leave it.
    void Combat::shoot(WorldState& state, const Content& content, std::uint32_t battle, int side)
    {
        Regiments& regiments = state.regiments;
        BattleSide& mine = state.battles.list[battle].sides[side];
        const CombatDefines& defines = content.combat;
        const int width = today_[battle].width;
        for (int i = 0; i < width; ++i) {
            const int position = order_at(i);
            const int r = regiment_of(mine.front[static_cast<std::size_t>(position)]);
            if (r >= 0) {
                fire_at(state, content, battle, side, position, static_cast<std::uint16_t>(r));
            }
        }
        for (int i = width; i < kMaxCombatWidth; ++i) {
            const int position = order_at(i);
            if (mine.front[static_cast<std::size_t>(position)] != 0) {
                to_reserve(regiments, mine, position);
            }
        }
        const Fixed enemy_max = today_[battle].sides[1 - side].max_morale;
        const Fixed line_loss = fx_mul(enemy_max, defines.daily_morale_damage);
        const Fixed reserve_loss = fx_mul(kFixedOne, fx_mul(enemy_max, defines.reserve_daily_morale_damage));
        for (const std::uint16_t ref : mine.front) {
            if (ref != 0) {
                damage_morale_[static_cast<std::size_t>(regiment_of(ref))] += line_loss;
            }
        }
        for_each_reserve(regiments, mine, [&](std::uint16_t r) {
            damage_morale_[r] += reserve_loss;
            if (regiments.strength[r] < 1 || regiments.morale[r] < kBroken) {
                reserve_remove(regiments, mine, r);
                regiments.place[r] = kPlaceOut;   // unable before today's damage
            }
        });
    }

    // EU4's CLandCombatant::FireAt, the operations in the game's order (BattleSim's fireAt); the factors that are 1
    // here (combat ability, the damage dealt and received, the morale damage) are left out, as multiplying by 1.000
    // changes nothing.
    void Combat::fire_at(const WorldState& state, const Content& content, std::uint32_t battle, int side, int position,
                         std::uint16_t regiment)
    {
        const Battle& b = state.battles.list[battle];
        const Regiments& regiments = state.regiments;
        const RegimentTypeNumbers& n = content.regiment_numbers;
        const CombatDefines& defines = content.combat;
        const BattleSide& enemy = b.sides[1 - side];
        const int target_position = best_target(regiments, enemy, position, maneuver(state, content, regiment));
        if (target_position < 0) {
            return;
        }
        const auto target = static_cast<std::uint16_t>(regiment_of(enemy.front[static_cast<std::size_t>(target_position)]));
        const bool flank = enemy.front[static_cast<std::size_t>(position)] == 0;   // nobody straight ahead: a flank attack

        const std::uint8_t phase = b.phase;
        const std::uint8_t mine = regiments.type[regiment];
        const std::uint8_t theirs = regiments.type[target];
        const int offence = phase == kPhaseFire ? n.off_fire[mine] : n.off_shock[mine];
        const int defence = phase == kPhaseFire ? n.def_fire[theirs] : n.def_shock[theirs];
        const int phase_pips = offence - defence;
        const int morale_pips = (n.off_morale[mine] - n.def_morale[theirs]) + phase_pips;
        const int dice = b.sides[side].dice;
        constexpr int kGlobalModifier = 0;   // generals, terrain, crossings: not yet
        const int x = std::clamp(phase_pips + kGlobalModifier + dice + 2, 0, 21);
        const int xm = std::clamp(morale_pips + kGlobalModifier + dice + 2, 0, 21);
        const Fixed base = 25 * x + 25;   // × 0.2 below: 15 + 5 × (dice + pips + 2)
        const Fixed base_morale = 25 * xm + 25;

        const SideDay& shooter = today_[battle].sides[side];
        const Fixed tactics = today_[battle].sides[1 - side].tactics;
        const RegimentCategory category = category_of(content, regiments, regiment);
        const int weapon_index = (category == RegimentCategory::cavalry ? 2 : 0) + (phase == kPhaseFire ? 0 : 1);
        const Fixed weapon =
            category == RegimentCategory::artillery ? 0 : shooter.weapon[static_cast<std::size_t>(weapon_index)];
        const Fixed discipline = shooter.discipline;
        const std::int32_t strength = regiments.strength[regiment];
        const Fixed range = defines.front_line_modifier;
        const Fixed length = kFixedOne + b.duration * defines.war_length_damage;

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
        Fixed morale = fx_div(shooter.max_morale, 2700);   // the shooter's country's maximum morale / 2.7
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

    // ------------------------------------------------------------------------------------------------ the outcome

    // EU4's HasMoraleCollapsed: the average morale of the regiments in the lines and the reserve with men and morale
    // (> 0.009) is not above 0.010, or there are none.
    bool Combat::morale_collapsed(std::uint32_t battle, int side) const
    {
        const SideDay& s = today_[battle].sides[side];
        if (s.fit == 0) {
            return true;
        }
        return !(10 < s.fit_morale / s.fit);
    }

    // EU4's CanMoraleCollapse: the average morale of every regiment of the side (the broken ones included) below
    // 0.25, or nobody able to fight.
    bool Combat::can_morale_collapse(const WorldState& state, const Content& content, std::uint32_t battle,
                                     int side) const
    {
        const SideDay& s = today_[battle].sides[side];
        if (s.regiments == 0) {
            return true;
        }
        return !(0xF9 < s.morale / s.regiments &&
                 able_to_fight(state.regiments, content, state.battles.list[battle].sides[side]));
    }

    // EU4's WillLose: under 10 men; or, once the battle can be left, its morale collapsed.
    bool Combat::will_lose(const WorldState& state, const Content& content, std::uint32_t battle, int side) const
    {
        if (today_[battle].sides[side].men < 10) {
            return true;
        }
        if (state.battles.list[battle].duration < content.combat.cannot_retreat_days) {
            return false;
        }
        return morale_collapsed(battle, side);
    }

    // EU4's CheckOverrun.
    int Combat::overrun(const WorldState& state, const Content& content, std::uint32_t battle, int factor,
                        bool daily) const
    {
        const Battle& b = state.battles.list[battle];
        const int width = today_[battle].width;
        std::int64_t men_a = today_[battle].sides[0].men;
        std::int64_t men_b = today_[battle].sides[1].men;
        if (!daily && men_a / 1000 >= width && men_b / 1000 >= width) {
            return -1;   // both fill the width
        }
        const bool first_day = b.phase_day == kNotDeployed;
        const bool able_a = able_to_fight(state.regiments, content, b.sides[0]);
        const bool able_b = able_to_fight(state.regiments, content, b.sides[1]);
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

    // ------------------------------------------------------------------------------------------------ the end

    void Combat::finish(WorldState& state, const Content& content, Movement& movement, std::uint32_t battle,
                        int winner, bool wiped)
    {
        Armies& armies = state.armies;
        Regiments& regiments = state.regiments;
        Battle& b = state.battles.list[battle];
        const int loser = 1 - winner;
        const std::uint16_t province = b.province;
        const std::uint16_t countries[2] = { b.sides[0].country, b.sides[1].country };

        // EU4's GiveEndOfCombatMoraleBoost: every regiment of the winner has at least its maximum morale ×
        // min(0.75, 1.25 × the loser's size / the winner's).
        const std::int64_t share = std::min<std::int64_t>(
            750, 1250 * std::int64_t{ b.joined[loser] } / std::max<std::int64_t>(1, b.joined[winner]));
        const Fixed floor = fx_mul(today_[battle].sides[winner].max_morale, share);

        // The regiments leave the battle: out of its lines and reserves.
        for_each_alive(regiments.alive, [&](std::uint32_t r) {
            const std::uint16_t a = regiments.army[r];
            if (armies.battle[a] != battle + 1) {
                return;
            }
            if (armies.side[a] == winner && regiments.strength[r] > 0) {
                regiments.morale[r] = std::max(regiments.morale[r], floor);
            }
            regiments.place[r] = kPlaceNone;
            regiments.reserve_prev[r] = 0;
            regiments.reserve_next[r] = 0;
        });
        // Its armies (all in its province), in slot order: the loser's retreat or are wiped out.
        std::uint16_t destination = 0;
        bool searched = false;
        for (std::uint32_t k = army_offsets_[province]; k < army_offsets_[province + 1]; ++k) {
            const std::uint16_t a = army_list_[k];
            if (!slot_alive(armies.alive, a) || armies.battle[a] != battle + 1) {
                continue;
            }
            const int side = armies.side[a];
            armies.battle[a] = 0;
            armies.side[a] = 0;
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
        log::info("battle in province {} after {} days: {} beats {}, who {}", province, b.duration,
                  content.countries.tag_text(countries[winner]), content.countries.tag_text(countries[loser]),
                  wiped ? "is wiped out" : destination != 0 ? "retreats" : "has nowhere to retreat");

        province_battle_[province] = 0;
        const std::uint16_t generation = b.generation;
        b = Battle{};
        b.generation = static_cast<std::uint16_t>(generation + 1);
        state.battles.alive[battle / 64] &= ~(std::uint64_t{ 1 } << (battle % 64));
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
