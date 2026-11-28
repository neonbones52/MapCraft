#include "game/recovery.h"

#include <algorithm>

#include "game/armies.h"
#include "game/modifiers.h"

namespace mc
{
    void Recovery::build()
    {
        gain_.assign(kArmySlots, 0);
        cap_.assign(kArmySlots, 0);
    }

    void Recovery::month(WorldState& state, const Content& content, const CountryModifiers& sums)
    {
        const Armies& armies = state.armies;
        const Provinces& provinces = state.provinces;
        const CombatDefines& defines = content.combat;
        // The share of each army, by where it stands; none in a battle.
        for_each_alive(armies.alive, [&](std::uint32_t a) {
            const std::uint16_t me = armies.owner[a];
            gain_[a] = 0;
            cap_[a] = 0;
            if (me == 0 || armies.battle[a] != 0) {
                return;
            }
            const std::uint16_t p = armies.province[a];
            Fixed share = defines.recovery_enemy_land;
            if ((armies.retreating[a / 64] >> (a % 64)) & 1u) {
                share = defines.recovery_retreating;
            } else if (provinces.owner[p] == me) {
                share = defines.recovery_own_land;
            } else if (provinces.owner[p] == 0 || provinces.controller[p] == me) {
                share = defines.recovery_neutral_land;
            }
            cap_[a] = max_morale(sums, me);
            gain_[a] = fx_mul(cap_[a], share);
        });
        // Each regiment of an army out of battle, up to its country's maximum.
        Regiments& regiments = state.regiments;
        for_each_alive(regiments.alive, [&](std::uint32_t r) {
            const std::uint16_t a = regiments.army[r];
            const Fixed morale = regiments.morale[r];
            if (morale < cap_[a]) {
                regiments.morale[r] = std::min(cap_[a], morale + gain_[a]);
            }
        });
    }
}
