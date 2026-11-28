#pragma once

#include <array>
#include <cstdint>

#include "core/fixed.h"
#include "game/game_data.h"

namespace mc
{
    class MapData;
    class Movement;

    // The land battle (docs/GAMEDATA.md, "Бой"), EU4 1.37.5's day after day in its order of operations (as BattleSim
    // reproduces eu4.exe: CCombat::DailyUpdate, CLandCombatant::FireAt, ArrangeBattleField, ReorganizeTroops,
    // CheckOverrun, WillLose), in Fixed (core/fixed.h). Enemies before diplomacy: every foreign army.
    //
    // The state is WorldState's: a battle with its two lines and reserves (Battle, game/game_data.h), its armies
    // (Armies::battle, side and joined, the retreating mask) and their generals (Armies::leader, Leaders), its
    // regiments (Regiments::place and the reserve's links). This
    // class holds the day's working data only, rebuilt each day from that state (so a load, a restart or a save change
    // nothing), sized once in build: no allocation in the tick.
    class Combat
    {
    public:
        // The working data's room, for the map's provinces. After every load.
        void build(const MapData& map);

        // One day of the battles, after the march (World::tick_day): retreats that arrived end; new battles where
        // foreign armies meet; armies of a battle's countries coming into its province join it; every battle fights
        // its day, in slot order; the ones decided end (the loser retreats or is wiped out). `day` and `seed` name the
        // dice (core/random.h).
        void day(WorldState& state, const Content& content, const CountryModifiers& sums, Movement& movement,
                 std::uint32_t seed, std::uint32_t day);

    private:
        // A side's numbers for the day: its country's (game/modifiers.h); its commander and the dice's global modifier
        // (command); its regiments' after the day's damage (tally): men; morale summed over every regiment with men
        // (count), and over the fit ones in the lines and the reserve (EU4's HasMoraleCollapsed).
        struct SideDay
        {
            int commander = -1;   // the general's slot, -1: none
            int global = 0;       // added to the dice of every shot (EU4's DetermineGlobalModifiers), < 0 for an
                                  // attacker into hard terrain or over a river
            Fixed discipline = 0;
            Fixed tactics = 0;
            Fixed max_morale = 0;
            std::array<Fixed, 6> weapon{};   // infantry, cavalry and artillery: fire, shock each
            std::int64_t men = 0;
            std::int64_t morale = 0;
            std::uint32_t regiments = 0;
            std::int64_t fit_morale = 0;
            std::uint32_t fit = 0;
        };
        // A battle's day: the width it fights at, its sides.
        struct BattleDay
        {
            int width = 0;
            std::array<SideDay, 2> sides{};
        };

        // The day's indices: live armies by province (CSR, slot order), regiments per army.
        void index(const WorldState& state);
        // Battles begin and armies join them.
        void meet(WorldState& state, const Content& content, Movement& movement, std::uint32_t day);
        void join(WorldState& state, Movement& movement, std::uint32_t battle, std::uint16_t army);
        // The sides' numbers of their countries and the width, for the day.
        void prepare(const WorldState& state, const Content& content, const CountryModifiers& sums);
        // The sides' men and morale over their regiments, after the day's damage.
        void tally(const WorldState& state);

        // A battle's day, before the damage lands (false: it ended before the first shot) and after.
        bool fight(WorldState& state, const Content& content, Movement& movement, std::uint32_t battle,
                   std::uint32_t seed, std::uint32_t day);
        void settle(WorldState& state, const Content& content, Movement& movement, std::uint32_t battle);
        // The sides' commanders and the dice's global modifiers, for the day's phase: the generals, the terrain and
        // the crossing.
        void command(const WorldState& state, const Content& content, std::uint32_t battle);
        void arrange(WorldState& state, const Content& content, std::uint32_t battle, int side);
        void shoot(WorldState& state, const Content& content, std::uint32_t battle, int side);
        // A regiment of a line at `position` shoots, `range` the line's (front_line_modifier, back_line_modifier).
        void fire_at(const WorldState& state, const Content& content, std::uint32_t battle, int side, int position,
                     std::uint16_t regiment, Fixed range);

        [[nodiscard]] bool morale_collapsed(std::uint32_t battle, int side) const;
        [[nodiscard]] bool can_morale_collapse(const WorldState& state, const Content& content, std::uint32_t battle,
                                               int side) const;
        [[nodiscard]] bool will_lose(const WorldState& state, const Content& content, std::uint32_t battle,
                                     int side) const;
        // The side that is overrun (EU4's CheckOverrun), -1 if none.
        [[nodiscard]] int overrun(const WorldState& state, const Content& content, std::uint32_t battle, int factor,
                                  bool daily) const;

        // The end: the winner's morale, the loser retreats or is wiped out, the battle's slot is freed.
        void finish(WorldState& state, const Content& content, Movement& movement, std::uint32_t battle, int winner,
                    bool wiped);
        [[nodiscard]] std::uint16_t retreat_to(const WorldState& state, Movement& movement, std::uint16_t country,
                                               std::uint16_t from) const;

        std::uint32_t province_count_ = 0;
        Column<std::uint32_t> army_offsets_;     // live armies by province: CSR over province ids
        Column<std::uint16_t> army_list_;
        Column<std::uint32_t> fill_;             // scratch of the counting sort
        Column<std::uint32_t> army_regiments_;   // live regiments per army slot (after the day's losses)
        Column<std::uint16_t> province_battle_;  // the battle in a province: slot + 1, 0 = none
        // Per regiment slot: the day's damage, taken while both sides shoot and applied at once after (EU4's
        // TakeDamage and ApplyDamage), zero between days.
        Column<std::int32_t> damage_men_;
        Column<Fixed> damage_morale_;
        Column<BattleDay> today_;   // per battle slot
        // Scratch of arrange: the infantry, the cavalry and the artillery it draws up, in the reserve's order.
        Column<std::uint16_t> centre_;
        Column<std::uint16_t> flank_;
        Column<std::uint16_t> second_;
    };
}
