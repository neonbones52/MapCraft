#pragma once

#include <bit>
#include <cstdint>

#include "game/game_data.h"

namespace mc
{
    // Armies and regiments as entities that are born and die (docs/SIMULATION.md, "Раскладка данных"): slots handed
    // out deterministically (the lowest free one), freed slots cleared, no allocation. The tables are game_data.h's
    // Armies and Regiments; these are the only functions that take or free a slot.

    // An army as the outside (a command, the bot, the UI) names it: its slot and the slot's generation then. Once the
    // army dies its slot's generation grows, so the reference no longer resolves, even after another army takes the
    // slot.
    struct ArmyRef
    {
        std::uint16_t slot = 0;
        std::uint16_t generation = 0;
    };

    [[nodiscard]] inline bool slot_alive(const Column<std::uint64_t>& alive, std::uint32_t slot)
    {
        return (alive[slot / 64] >> (slot % 64)) & 1u;
    }

    // f(slot) for every live slot, in slot order.
    template <class F>
    void for_each_alive(const Column<std::uint64_t>& alive, F&& f)
    {
        for (std::uint32_t w = 0; w < alive.size(); ++w) {
            for (std::uint64_t bits = alive[w]; bits != 0; bits &= bits - 1) {
                f(w * 64 + static_cast<std::uint32_t>(std::countr_zero(bits)));
            }
        }
    }

    [[nodiscard]] ArmyRef army_ref(const Armies& armies, std::uint16_t slot);
    // The army's slot if the reference still names a live army.
    [[nodiscard]] bool resolve(const Armies& armies, ArmyRef ref, std::uint16_t* slot);

    // A new army in the lowest free slot; false when every slot is taken.
    bool create_army(Armies& armies, std::uint16_t owner, std::uint16_t province, std::uint16_t* slot);
    // The army and all its regiments die; their slots are cleared, the army's generation grows.
    void destroy_army(WorldState& state, std::uint16_t slot);

    // A new regiment of the army in the lowest free slot, `strength` men at full morale `morale` (Fixed); false when
    // every slot is taken.
    bool add_regiment(Regiments& regiments, std::uint16_t army, std::uint8_t type, std::int32_t strength, Fixed morale,
                      std::uint16_t* slot);
    void remove_regiment(Regiments& regiments, std::uint16_t slot);

    // The army's regiments and their men (a query for the UI and the checks).
    struct ArmyTotals
    {
        std::uint32_t regiments = 0;
        std::int64_t men = 0;
        Fixed morale = 0;   // the regiments' average, truncated
    };
    [[nodiscard]] ArmyTotals army_totals(const Regiments& regiments, std::uint16_t army);
}
