#include "game/recovery.h"

#include <algorithm>

#include "game/armies.h"
#include "game/economy/upkeep.h"
#include "game/modifiers.h"
#include "game/war.h"

namespace mc
{
    void Recovery::build()
    {
        gain_.assign(kArmySlots, 0);
        cap_.assign(kArmySlots, 0);
        ceiling_.assign(kArmySlots, kNoMoraleCeiling);
    }

    void Recovery::month(WorldState& state, const Content& content, const Cache& cache)
    {
        const CountryModifiers& sums = cache.country_modifiers;
        const Armies& armies = state.armies;
        const Provinces& provinces = state.provinces;
        const CombatDefines& defines = content.combat;
        // The share of each army, by where it stands; none in a battle. The ceiling its country's maintenance sets
        // (game/economy/upkeep.h), in a battle too.
        for_each_alive(armies.alive, [&](std::uint32_t a) {
            const std::uint16_t me = armies.owner[a];
            gain_[a] = 0;
            cap_[a] = 0;
            ceiling_[a] = morale_ceiling(sums, state.countries, content.economy, me);
            if (me == 0 || armies.battle[a] != 0) {
                return;
            }
            const std::uint16_t p = armies.province[a];
            const std::uint16_t owner = provinces.owner[p];
            Fixed share = defines.recovery_neutral_land;
            if ((armies.retreating[a / 64] >> (a % 64)) & 1u) {
                share = defines.recovery_retreating;
            } else if (owner == me) {
                share = defines.recovery_own_land;
            } else if (at_war(cache.at_war, me, owner) && provinces.controller[p] != me) {
                share = defines.recovery_enemy_land;
            }
            const Fixed full = max_morale(sums, me);
            cap_[a] = std::min(full, ceiling_[a]);
            gain_[a] = fx_mul(full, share);
        });
        // Each regiment: cut down to its ceiling, then, out of battle, up to its country's maximum or the ceiling.
        Regiments& regiments = state.regiments;
        for_each_alive(regiments.alive, [&](std::uint32_t r) {
            const std::uint16_t a = regiments.army[r];
            const Fixed morale = std::min(regiments.morale[r], ceiling_[a]);
            regiments.morale[r] = morale < cap_[a] ? std::min(cap_[a], morale + gain_[a]) : morale;
        });
    }
}
