#pragma once

#include <cstdint>

#include "game/game_data.h"

namespace mc
{
    // Wars (docs/GAMEDATA.md, "Война"): the state is Wars and Truces (game/game_data.h), the cache AtWar. A war is
    // between two countries, the one that declared it and the one it was declared on (no alliances yet); it begins
    // with the command "declare war" (no casus belli, no infamy) and ends with a peace (game/peace.h), which leaves a
    // truce: until it runs out, the two cannot declare war on each other. Enemies are the countries at war: only they
    // fight battles (game/combat.h), and an enemy's land is the one of a country at war with the army's
    // (game/recovery.h).

    // Countries `a` and `b` are at war.
    [[nodiscard]] inline bool at_war(const AtWar& at_war, std::size_t a, std::size_t b)
    {
        return at_war.has(a, b);
    }

    // The slot of the war between `a` and `b`, -1 if they are not at war.
    [[nodiscard]] int war_between(const Wars& wars, std::uint16_t a, std::uint16_t b);
    // The country that declared the war between `a` and `b`, 0 if they are not at war.
    [[nodiscard]] std::uint16_t war_declarer(const Wars& wars, std::uint16_t a, std::uint16_t b);

    // The cache from the wars, for `country_count` countries (0 = none included). After every load and restart
    // (GameData::rebuild_cache), and when a war ends.
    void rebuild_at_war(const Wars& wars, std::size_t country_count, AtWar* out);

    // The first day `a` and `b` may declare war on each other again, 0 if there is no truce between them (or it ran
    // out by `day`).
    [[nodiscard]] std::uint32_t truce_until(const Truces& truces, std::uint16_t a, std::uint16_t b, std::uint32_t day);

    // Why `attacker` cannot declare war on `defender` on `day` (no such country, itself, already at war, a truce, no
    // war slot left), or nullptr if it can.
    [[nodiscard]] const char* why_no_war(const WorldState& state, const AtWar& at_war, std::uint16_t attacker,
                                         std::uint16_t defender, std::uint32_t day);
    // The command's effect (World): `attacker` declares war on `defender` on `day`, if why_no_war says nothing; the
    // war takes the lowest free slot and the cache is updated with it. False, and nothing changes, otherwise.
    bool declare_war(WorldState& state, AtWar& at_war, std::uint16_t attacker, std::uint16_t defender,
                     std::uint32_t day);

    // The war in `slot` ends (a peace's part, game/peace.h): its slot is cleared, its generation grows, and the cache
    // is rebuilt.
    void end_war(WorldState& state, AtWar& at_war, std::uint16_t slot);
    // A truce between `a` and `b` until `until` (a day): it takes the lowest free slot, or replaces theirs; with no
    // slot left, the one that runs out first gives way (lower slot on a tie).
    void set_truce(Truces& truces, std::uint16_t a, std::uint16_t b, std::uint32_t until);
    // The truces that ran out by `day` free their slots (World::tick_month).
    void expire_truces(Truces& truces, std::uint32_t day);
}
