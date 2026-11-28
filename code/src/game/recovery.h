#pragma once

#include "game/game_data.h"

namespace mc
{
    // The armies' recovery out of battle, each month (World::tick_month; docs/GAMEDATA.md, "Восстановление"): a
    // regiment not in a battle gets back a share of its country's maximum morale (game/modifiers.h), up to that
    // maximum, never above it and never taken down from above it. Its country's maintenance below 100 sets a lower
    // ceiling (game/economy/upkeep.h): every regiment's morale, in a battle or not, is first cut down to it, and the
    // recovery fills only up to it. The share is the army's, by where it stands (the
    // user's decision; defines.csv):
    //   recovery_own_land       its country owns the province
    //   recovery_neutral_land   no one owns it, a country at peace with its country owns it, or its country controls
    //                           it without owning it (occupied)
    //   recovery_enemy_land     a country at war with its country owns it (game/war.h), and its country does not
    //                           control it
    //   recovery_retreating     it retreats from a lost battle, wherever it is
    // The men come back by the same shares later (M8). Owned by World; its data is the month's working column only.
    class Recovery
    {
    public:
        // The working column's room. After every load.
        void build();
        // One month's recovery.
        void month(WorldState& state, const Content& content, const Cache& cache);

    private:
        Column<Fixed> gain_;   // per army slot: the morale each of its regiments gets back this month
        Column<Fixed> cap_;    // per army slot: what its regiments recover up to: its country's maximum morale, or
                               // the maintenance's ceiling below it
        Column<Fixed> ceiling_;   // per army slot: its country's maintenance ceiling (kNoMoraleCeiling at 100)
    };
}
