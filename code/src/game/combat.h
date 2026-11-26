#pragma once

#include <cstddef>
#include <cstdint>

#include "core/fixed.h"
#include "game/game_data.h"

namespace mc
{
    class MapData;
    class Movement;

    // The land battle (docs/GAMEDATA.md, "Бой"), EU4 1.37.5's day after day in its order of operations (as BattleSim
    // reproduces eu4.exe: CCombat::DailyUpdate, CLandCombatant::FireAt, ArrangeBattleField, ReorganizeTroops,
    // CheckOverrun, WillLose), in Fixed (core/fixed.h). Enemies before diplomacy: every foreign army. A battle has two
    // sides, a country each: the defender stood in the province, the attacker came in.
    //
    // The state is WorldState's (Battles; Armies::battle and retreating; Regiments::place and queue); this class holds
    // the rules' working data, rebuilt from that state at the start of every day's battles (so a load, a restart or a
    // save change nothing), sized once in build: no allocation in the tick.
    class Combat
    {
    public:
        // The working columns' room, for the map's provinces. After every load.
        void build(const MapData& map);

        // One day of the battles, after the march (World::tick_day): retreats that arrived end; new battles where
        // foreign armies meet; armies of a battle's countries coming into its province join it; every battle fights
        // its day, in slot order; the ones decided end (the loser retreats or is wiped out). `day` and `seed` name the
        // dice (core/random.h).
        void day(WorldState& state, const Content& content, const CountryModifiers& sums, Movement& movement,
                 std::uint32_t seed, std::uint32_t day);

    private:
        static constexpr std::uint16_t kEmpty = 0xFFFF;

        [[nodiscard]] static std::size_t side_index(std::uint32_t battle, int side) { return battle * 2u + side; }
        [[nodiscard]] std::uint16_t& front(std::uint32_t battle, int side, int position)
        {
            return front_[side_index(battle, side) * kFrontSlots + static_cast<std::size_t>(position)];
        }
        [[nodiscard]] std::uint16_t front(std::uint32_t battle, int side, int position) const
        {
            return front_[side_index(battle, side) * kFrontSlots + static_cast<std::size_t>(position)];
        }

        // The day's indices: live armies by province, regiments by army (both CSR, slot order).
        void index(const WorldState& state);
        // Battles begin and armies join them.
        void meet(WorldState& state, const Content& content, Movement& movement, std::uint32_t day);
        void join(WorldState& state, Movement& movement, std::uint32_t battle, std::uint16_t army);
        // The lines and the reserve queues from the regiments' places, the sides' constants for the day.
        void rebuild(const WorldState& state, const Content& content, const CountryModifiers& sums);
        // The sides' men and morale over their regiments, after the day's damage.
        void tally(const WorldState& state);

        // A battle's day, before the damage lands (false: it ended before the first shot) and after.
        bool fight(WorldState& state, const Content& content, Movement& movement, std::uint32_t battle,
                   std::uint32_t seed, std::uint32_t day);
        void settle(WorldState& state, const Content& content, Movement& movement, std::uint32_t battle);
        void roll_dice(WorldState& state, const Content& content, std::uint32_t battle, std::uint32_t seed,
                       std::uint32_t day);
        void arrange(WorldState& state, const Content& content, std::uint32_t battle, int side);
        void reinforce(WorldState& state, const Content& content, std::uint32_t battle, int side);
        void shuffle_to_middle(WorldState& state, std::uint32_t battle, int side);
        void reorganize(WorldState& state, const Content& content, std::uint32_t battle, int side);
        void shoot(WorldState& state, const Content& content, std::uint32_t battle, int side);
        void fire_at(const WorldState& state, const Content& content, std::uint32_t battle, int side, int position,
                     std::uint16_t regiment);

        // Queries.
        [[nodiscard]] int maneuver(const WorldState& state, const Content& content, std::uint16_t regiment) const;
        [[nodiscard]] int best_target(const WorldState& state, std::uint32_t battle, int side, int position,
                                      int range) const;
        [[nodiscard]] int first_reserve(const WorldState& state, const Content& content, std::uint32_t battle,
                                        int side) const;
        [[nodiscard]] bool able_to_fight(const WorldState& state, const Content& content, std::uint32_t battle,
                                         int side) const;
        [[nodiscard]] bool morale_collapsed(std::uint32_t battle, int side) const;
        [[nodiscard]] bool can_morale_collapse(const WorldState& state, const Content& content, std::uint32_t battle,
                                               int side) const;
        [[nodiscard]] bool will_lose(const WorldState& state, const Content& content, std::uint32_t battle,
                                     int side) const;
        // The side that is overrun (EU4's CheckOverrun), -1 if none.
        [[nodiscard]] int overrun(const WorldState& state, const Content& content, std::uint32_t battle, int factor,
                                  bool daily) const;

        // The reserve queue of a side: entries [begin, end) of the pool, each a regiment and the queue number it had
        // when it went in; an entry stands while its regiment is still in the reserve with that number (moving out,
        // or in again later with a new number, leaves the old entry dead).
        void push_reserve(WorldState& state, std::uint32_t battle, int side, std::uint16_t regiment);
        [[nodiscard]] bool entry_live(const WorldState& state, std::uint32_t k) const;
        void compact_reserve(const WorldState& state, std::size_t side);

        // The end: the winner's morale, the loser retreats or is wiped out, the battle's slot is freed.
        void finish(WorldState& state, const Content& content, Movement& movement, std::uint32_t battle, int winner,
                    bool wiped);
        [[nodiscard]] std::uint16_t retreat_to(const WorldState& state, Movement& movement, std::uint16_t country,
                                               std::uint16_t from) const;

        std::uint32_t province_count_ = 0;
        // Live armies by province (CSR over province ids), regiments by army slot (CSR over army slots).
        Column<std::uint32_t> army_offsets_;
        Column<std::uint16_t> army_list_;
        Column<std::uint32_t> regiment_offsets_;
        Column<std::uint16_t> regiment_list_;
        Column<std::uint32_t> army_regiments_;   // live regiments per army slot (after the day's losses)
        Column<std::uint16_t> province_battle_;  // the battle in a province: slot + 1, 0 = none
        Column<std::uint32_t> fill_;             // scratch of the counting sorts

        // Per battle × side (side_index), kFrontSlots positions each: the regiment in a position, kEmpty if none.
        Column<std::uint16_t> front_;
        // The reserve queues: a pool, per side its range and its room.
        Column<std::uint16_t> reserve_regiment_;
        Column<std::uint32_t> reserve_queue_;
        Column<std::uint32_t> reserve_begin_;
        Column<std::uint32_t> reserve_end_;
        Column<std::uint32_t> reserve_room_;
        // Per regiment slot: the day's damage, taken while both sides shoot and applied at once after (EU4's
        // TakeDamage and ApplyDamage), zero between days.
        Column<std::int32_t> damage_men_;
        Column<Fixed> damage_morale_;
        // Per battle × side, for the day: the country's numbers (game/modifiers.h).
        Column<Fixed> side_discipline_;
        Column<Fixed> side_tactics_;
        Column<Fixed> side_max_morale_;
        Column<Fixed> side_weapon_;   // 4 per side: infantry fire, infantry shock, cavalry fire, cavalry shock
        // Per battle × side, after the day's damage (tally): men; morale summed over every regiment with men (count),
        // and over the fit ones in the lines and the reserve (EU4's HasMoraleCollapsed).
        Column<std::int64_t> side_men_;
        Column<std::int64_t> side_morale_;
        Column<std::uint32_t> side_regiments_;
        Column<std::int64_t> side_fit_morale_;
        Column<std::uint32_t> side_fit_;
        // Per battle: the width it fights at today.
        Column<std::uint8_t> width_;
        // Scratch of arrange: the infantry and the cavalry it draws up, in queue order.
        Column<std::uint16_t> centre_;
        Column<std::uint16_t> flank_;
    };
}
