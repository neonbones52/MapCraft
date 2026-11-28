#pragma once

#include <array>
#include <cstdint>

#include "core/fixed.h"
#include "game/game_data.h"

namespace mc
{
    class MapData;

    // Occupation (docs/GAMEDATA.md, "Осада и оккупация"): the sieges, the garrisons and who controls a province, by
    // EU4 1.37.5's CSiegeCombat (TryToSiege, DailyUpdate, TickSiegePhase, CalcSiegeImpact, GetArtilleryValue,
    // GetDaysPerSiegePhase, GetMaxSiegeMemory; C:\Dev\Projects\eu4\archeologyEU4\UtilityAI\evidence) and its wiki.
    //
    // A province is besieged by a country that stands in it with its armies, standing (not marching) and not
    // retreating, when no battle goes on there and the country is at war with its controller, and owns it or is at
    // war with its owner too (a country retakes its own land by a siege as well); with at least siege_men_needed men
    // (the user's decision: no fewer than siege_min_men, 1000, even where there is no garrison; a fort's garrison
    // asks for siege_force_needed_multiplier × its maximum). Too few men, no armies left, the war's end: the siege
    // ends, its breaches and its days with it; its progress stays with the province and goes down 1 a day.
    // A battle in the province holds the siege's days; its armies in the battle still besiege.
    // A siege's day: days + 1; at the phase's length (days_per_siege_phase × the modifiers, phase_days) a phase: the
    // die, 1..siege_dice_sides, + the impact (siege_numbers): a disease on a 1, a breach, or a result by the table of
    // EU4 (more progress, the garrison thinner), or the fort surrenders; a garrison below siege_garrison_surrender
    // surrenders at the next phase, and a province without a fort has none: it falls at its first phase.
    // The fort surrenders: the besieger controls the province (its owner, if the besieger owns it: freed), its
    // progress is 0, its garrison garrison_after_capture.
    // Garrisons: full at the start, each month out of a siege up by garrison_growth of the maximum (month()).
    //
    // The state is WorldState's: Sieges (the slots), Provinces::controller, fort, garrison and siege_progress. This
    // class holds the day's working data only, rebuilt each day from that state, sized once in build: no allocation
    // in the tick.

    // The province's fort level: its fort, + capital_fort_level if it is its owner's seat (game/countries.h).
    [[nodiscard]] int fort_level(const WorldState& state, const Content& content, std::uint16_t province);
    // Its garrison's maximum: fort level × garrison_size; 0 without a fort.
    [[nodiscard]] std::int32_t max_garrison(const Content& content, int fort_level);
    // The men a siege of the province needs (EU4's GetRegimentsNeededForSiege, in men; at least siege_min_men).
    [[nodiscard]] std::int64_t siege_men_needed(const WorldState& state, const Content& content,
                                                std::uint16_t province);
    // Whether `country` may besiege the province: it has an owner and a controller other than `country`, the country
    // is at war with the controller and owns the province or is at war with its owner too.
    [[nodiscard]] bool may_besiege(const WorldState& state, const AtWar& at_war, std::uint16_t country,
                                   std::uint16_t province);
    // Whether the province has a port (MapData's port point): its fort is not blockaded, there being no fleets.
    [[nodiscard]] bool has_port(const MapData& map, std::uint16_t province);
    // Every garrison full: at the load (GameData::load).
    void fill_garrisons(WorldState& state, const Content& content);

    // A siege's besiegers today: the besieging country's armies in the province, standing and not retreating.
    struct Besiegers
    {
        std::int64_t men = 0;
        std::int64_t artillery = 0;   // the men of their artillery regiments
        int commander = -1;           // EU4's CalcHighestCommander: the general with the most fire + shock + 1 (equal
                                      // ones: the lower army slot), its slot; -1 none
        int commander_score = -1;     // its fire + shock + 1
    };
    // The besiegers of the siege in `slot`, over the armies and regiments (the UI's; the rule keeps its own sums).
    [[nodiscard]] Besiegers besiegers(const WorldState& state, const Content& content, std::uint16_t slot);

    // A siege's numbers today (EU4's CalcSiegeImpact and its parts, GetDaysPerSiegePhase, GetMaxSiegeMemory).
    struct SiegeNumbers
    {
        int fort = 0;                  // the fort level
        std::int32_t max_garrison = 0;
        std::int64_t needed = 0;       // the men it needs
        int leader = 0;                // the commander's siege pips
        int artillery = 0;             // the artillery's bonus: min(artillery_vs_fort, artillery's regiments ×
                                       // 1000 men / (fort / 2 + 1)), at least 1 with any artillery
        int blockade = 0;              // -siege_unblockaded_penalty at a fort with a port, else 0
        int obsolete = 0;              // (the besieger's highest_fort - max(fort, 2)) / 2, not below 0
        int low_garrison = 0;          // 1 below half the maximum garrison
        int impact = 0;                // the sum the roll gets: progress + leader + breaches + artillery + blockade
                                       // - fort + obsolete + low_garrison; 0 without a fort
        int phase_days = 0;            // the phase's length
        int progress_cap = 0;          // siege_memory + siege_memory_scaled × highest_fort
    };
    [[nodiscard]] SiegeNumbers siege_numbers(const WorldState& state, const Content& content, const Cache& cache,
                                             bool port, std::uint16_t slot, const Besiegers& besiegers);

    // What a phase's roll comes to (EU4's TickSiegePhase), from the siege's numbers, its breaches and the garrison's
    // men: the roll as kept (a 1 that would surrender is a 2), the result, the progress it gives, the garrison's loss
    // (‰ of its men). One function for the rule and the UI's chances.
    struct SiegeOutcome
    {
        int roll = 0;
        SiegeResult result = SiegeResult::status_quo;
        bool surrender = false;
        int progress = 0;
        std::int64_t loss = 0;
    };
    [[nodiscard]] SiegeOutcome siege_outcome(const CombatDefines& defines, const SiegeNumbers& numbers, int breaches,
                                             std::int32_t garrison, int roll);

    // The chances of the next phase (EU4's siege tooltip): of the siege_dice_sides faces of the die, how many give
    // each result and how many make the fort surrender.
    struct SiegeChances
    {
        int sides = 0;
        int surrender = 0;
        std::array<int, 8> result{};   // per SiegeResult (the surrender's faces are not among them)
    };
    [[nodiscard]] SiegeChances siege_chances(const CombatDefines& defines, const SiegeNumbers& numbers, int breaches,
                                             std::int32_t garrison);

    class Occupation
    {
    public:
        // The working data's room and the ports, for the map's provinces and the countries. After every load.
        void build(const MapData& map, const Content& content);

        // One day of the sieges, after the battles (World::tick_day): the sieges whose besiegers are too few or gone
        // end; the others count their day (but where a battle goes on), and a phase where one is due; new sieges
        // begin; the progress of the provinces no one besieges goes down. `day` and `seed` name the dice.
        void day(WorldState& state, const Content& content, const Cache& cache, std::uint32_t seed,
                 std::uint32_t day);
        // One month of the garrisons (World::tick_month): each fort's not besieged gets garrison_growth of its
        // maximum back, up to it.
        void month(WorldState& state, const Content& content);

    private:
        void phase(WorldState& state, const Content& content, const Cache& cache, std::uint16_t slot,
                   std::uint32_t seed, std::uint32_t day);
        void begin(WorldState& state, std::uint16_t province, std::uint16_t country);
        void end(WorldState& state, std::uint16_t slot);

        std::uint32_t province_count_ = 0;
        std::uint32_t stamp_ = 0;              // grows every day: the marks below are today's when equal to it
        Column<std::uint8_t> port_;            // per province: 1 has a port
        Column<std::uint32_t> battle_mark_;    // per province: a battle goes on there today
        Column<std::uint32_t> siege_mark_;     // per province: a siege goes on there (siege_at_ valid)
        Column<std::uint16_t> siege_at_;       // per province: its siege's slot
        Column<std::uint32_t> candidate_mark_; // per province: a country may begin a siege there today
        Column<std::uint16_t> candidate_;      // per province: that country (the first army's by slot)
        Column<std::int64_t> candidate_men_;   // per province: its men there
        Column<std::uint16_t> candidates_;     // the provinces of today's candidates, in the order found
        Column<std::int64_t> army_men_;        // per army slot: its men today
        Column<std::int64_t> army_artillery_;  // per army slot: the men of its artillery
        Column<Besiegers> besiegers_;          // per siege slot: today's
        Column<std::uint32_t> seat_;           // per country: its seat (month())
    };
}
