#pragma once

#include <cstdint>

#include "core/money.h"
#include "game/game_data.h"

// The recruitment (docs/GAMEDATA.md, "Людские ресурсы и найм"; EU4's ways, the user's decisions 28.11.2026):
//   the order   the command recruit: a country raises a regiment of a type in a province it owns and controls and
//               no one besieges. It pays at once: the type's cost × (1 + inflation / 100) out of its treasury
//               (ledger::pay) and kFullRegiment men out of its manpower pool (game/economy/manpower.h). Without
//               enough of either the order is refused: the treasury never goes below 0 for it.
//   the queue   a province raises one regiment at a time, in the order they were ordered (Recruits::place): the
//               first one's days grow by 1 each day, from the day it was ordered; at its category's time
//               (infantry_time, cavalry_time, artillery_time) it stands in the province as a new army of one full
//               regiment at recruit_morale of its country's maximum morale (no more than the maintenance's ceiling,
//               game/economy/upkeep.h), and the next one begins the next day. With no army or regiment slot free it
//               waits, done, until there is one.
//   the siege   a siege of the province holds its queue (no day on) while it lasts; a battle there between the
//               besieger and the recruiter lets it go on; a siege lifted (the besiegers gone, or retreating after
//               a battle) ends the pause (the user's decision).
//   lost        a province its recruiter no longer owns or controls (occupied, given away in a peace) drops that
//               country's queue there: the money and the men are not given back.

namespace mc
{
    struct Cache;

    // The days a regiment of `type` takes to raise.
    [[nodiscard]] int recruit_time(const Content& content, std::uint8_t type);
    // What `country` pays for a regiment of `type` now.
    [[nodiscard]] Money recruit_cost(const WorldState& state, const Content& content, std::uint16_t country,
                                     std::uint8_t type);
    // Why `country` cannot recruit a regiment of `type` in `province` (the command recruit): no such country or
    // type, a province it does not own or control, one besieged, its queue full, too little money in the treasury
    // or too few men in the pool, no slot for the order; nullptr if it can.
    [[nodiscard]] const char* why_no_recruit(const WorldState& state, const Content& content, std::uint16_t country,
                                             std::uint16_t province, std::uint8_t type);
    // Whether a siege holds the queue of `country` in `province` now (the rule above; the UI shows it too).
    [[nodiscard]] bool recruitment_paused(const WorldState& state, std::uint16_t province, std::uint16_t country);
    // Orders it (checked by why_no_recruit first): pays and queues it.
    void recruit(WorldState& state, const Content& content, std::uint16_t country, std::uint16_t province,
                 std::uint8_t type);

    // One day of the queues (World::tick_day, after the sieges): the lost ones dropped, then each province's first
    // regiment a day on, unless a siege holds it; the regiments done stand as new armies, in slot order. `day`
    // names it in the log. Owned by World; its data is the day's working list.
    class Recruitment
    {
    public:
        void day(WorldState& state, const Content& content, const Cache& cache, std::uint32_t day);

    private:
        Column<std::uint16_t> work_;   // the recruits to drop, then the ones done
    };
}
