#pragma once

#include "game/game_data.h"

namespace mc
{
    // The armies' recovery out of battle, each month (World::tick_month; docs/GAMEDATA.md, "Восстановление"): a
    // regiment not in a battle gets back a share of its country's maximum morale (game/modifiers.h), up to that
    // maximum, never above it and never taken down from above it. The share is the army's, by where it stands (the
    // user's decision; defines.csv):
    //   recovery_own_land       its country owns the province
    //   recovery_neutral_land   no one owns it, or its country controls it without owning it (occupied)
    //   recovery_enemy_land     another country owns it and its country does not control it (before diplomacy
    //                           every other country is an enemy)
    //   recovery_retreating     it retreats from a lost battle, wherever it is
    // The men come back by the same shares later (M8). Owned by World; its data is the month's working column only.
    class Recovery
    {
    public:
        // The working column's room. After every load.
        void build();
        // One month's recovery.
        void month(WorldState& state, const Content& content, const CountryModifiers& sums);

    private:
        Column<Fixed> gain_;   // per army slot: the morale each of its regiments gets back this month
        Column<Fixed> cap_;    // per army slot: its country's maximum morale
    };
}
