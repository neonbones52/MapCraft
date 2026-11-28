#pragma once

#include <cstdint>

#include "core/fixed.h"
#include "game/game_data.h"

namespace mc
{
    // The war score (docs/GAMEDATA.md, "Военный счёт"), EU4 1.37.5's CWar::GetWarScore in its order and its integer
    // arithmetic (C:\Dev\Projects\eu4\archeologyEU4\UtilityAI\research\PEACE.md; eu4.exe: GetWarScore 0x140D90520,
    // GetBattleScore 0x140D8FD00, GetOccupiedScore 0x140D8E9A0, CalculateBattleScore 0x1401DD5D0, GetWarValue
    // 0x140A18310), from what the game has: the battles and the occupation, no blockade (no fleets) and no war goal's
    // ticking score (no casus belli until M11). A war score is a whole number, -max_warscore..max_warscore, + for the
    // side it is of. No state of its own: computed from WorldState on demand (the battles' sum is the war's,
    // Wars::battle_score).

    // What a country's provinces' values depend on: its seat (game/countries.h), its forts (provinces of a fort level
    // >= 1, game/occupation.h), its development (the sum of its base_tax: EU4's tax + production + manpower, only the
    // tax here), and the sum of its provinces' values (EU4's _nTotalWarWorth, whole points).
    struct CountryWarFacts
    {
        std::uint32_t seat = 0;
        int forts = 0;
        Fixed development = 0;
        std::int32_t total_value = 0;
    };
    [[nodiscard]] CountryWarFacts country_war_facts(const WorldState& state, const Content& content,
                                                    std::uint16_t country);

    // A province's war value (EU4's GetWarValue with its "no fort" flag): province_value_base + its development
    // (base_tax) up to province_value_cap; × capital_value_mult at its owner's seat; without a fort (fort level < 1)
    // × 0.1 if its owner has 10 forts or more, else × (4 - 3r) / 10, r = min(1, forts × fort_per_dev_ratio × 2 / the
    // owner's development) (0.4 at no development); then 1..100. Its price in a peace (game/peace.h). `owner`: the
    // facts of its owner.
    [[nodiscard]] Fixed province_value(const WorldState& state, const Content& content, std::uint16_t province,
                                       const CountryWarFacts& owner);

    // A land battle's score for its winner (EU4's CalculateBattleScore): the loser's losses in whole regiments (1000
    // men) over max(2 × its land force limit, game/economy/upkeep.h, 2 × those regiments), × warscore_max_from_battles / 2, at least 0.1.
    [[nodiscard]] Fixed battle_score(const WarDefines& defines, Fixed loser_force_limit, std::int64_t losses);
    // A battle won by `winner` over `loser`, who lost `losses` men: its score goes to their war (Wars::battle_score;
    // nothing if they are not at war). Returns the score (0 for no war). Combat's, at a battle's end.
    Fixed add_battle_score(WorldState& state, const Content& content, std::uint16_t winner, std::uint16_t loser,
                           std::int64_t losses);

    // A war's score and its parts, from its attacker's side.
    struct WarScore
    {
        int battles = 0;             // the battles' sum, whole points, within ±warscore_max_from_battles
        int attacker_occupation = 0; // what the attacker controls of the defender's: Σ value / its total × 100
        int defender_occupation = 0; // what the defender controls of the attacker's
        int complete = 0;            // +1: the attacker controls every province of the defender (a complete
                                     // victory, ±max_warscore); -1: the defender every one of the attacker's; 0 none
        int total = 0;               // the war score: ±max_warscore at a complete victory, else the battles
                                     // (within ±max_warscore) + attacker_occupation - defender_occupation, within
                                     // ±(max_warscore - 1)
    };
    [[nodiscard]] WarScore war_score(const WorldState& state, const Content& content, std::uint16_t war);
    // The war score of `country` in its war with `enemy` (+ when it is winning), 0 if they are not at war.
    [[nodiscard]] int war_score_of(const WorldState& state, const Content& content, std::uint16_t country,
                                   std::uint16_t enemy);
}
