#include "game/armies.h"

namespace mc
{
    namespace
    {
        // The lowest free slot of a mask, or false.
        bool take_slot(Column<std::uint64_t>& alive, std::uint16_t* slot)
        {
            for (std::uint32_t w = 0; w < alive.size(); ++w) {
                if (alive[w] != ~std::uint64_t{ 0 }) {
                    const int bit = std::countr_one(alive[w]);
                    alive[w] |= std::uint64_t{ 1 } << bit;
                    *slot = static_cast<std::uint16_t>(w * 64 + static_cast<std::uint32_t>(bit));
                    return true;
                }
            }
            return false;
        }

        void free_slot(Column<std::uint64_t>& alive, std::uint32_t slot)
        {
            alive[slot / 64] &= ~(std::uint64_t{ 1 } << (slot % 64));
        }
    }

    ArmyRef army_ref(const Armies& armies, std::uint16_t slot)
    {
        return { slot, armies.generation[slot] };
    }

    bool resolve(const Armies& armies, ArmyRef ref, std::uint16_t* slot)
    {
        if (ref.slot >= kArmySlots || !slot_alive(armies.alive, ref.slot) ||
            armies.generation[ref.slot] != ref.generation) {
            return false;
        }
        *slot = ref.slot;
        return true;
    }

    bool create_army(Armies& armies, std::uint16_t owner, std::uint16_t province, std::uint16_t* slot)
    {
        if (!take_slot(armies.alive, slot)) {
            return false;
        }
        armies.owner[*slot] = owner;
        armies.province[*slot] = province;
        return true;
    }

    void destroy_army(WorldState& state, std::uint16_t slot)
    {
        Regiments& regiments = state.regiments;
        for_each_alive(regiments.alive, [&](std::uint32_t r) {
            if (regiments.army[r] == slot) {
                remove_regiment(regiments, static_cast<std::uint16_t>(r));
            }
        });
        Armies& armies = state.armies;
        const std::uint16_t generation = armies.generation[slot];
#define MC_CLEAR_SLOT(table, type, name, start) armies.name[slot] = type{};
        MC_ARMY_COLUMNS(MC_CLEAR_SLOT)
#undef MC_CLEAR_SLOT
        armies.generation[slot] = static_cast<std::uint16_t>(generation + 1);
        free_slot(armies.alive, slot);
    }

    bool add_regiment(Regiments& regiments, std::uint16_t army, std::uint8_t type, float strength, float morale,
                      std::uint16_t* slot)
    {
        if (!take_slot(regiments.alive, slot)) {
            return false;
        }
        regiments.army[*slot] = army;
        regiments.type[*slot] = type;
        regiments.strength[*slot] = strength;
        regiments.morale[*slot] = morale;
        return true;
    }

    void remove_regiment(Regiments& regiments, std::uint16_t slot)
    {
#define MC_CLEAR_SLOT(table, type, name, start) regiments.name[slot] = type{};
        MC_REGIMENT_COLUMNS(MC_CLEAR_SLOT)
#undef MC_CLEAR_SLOT
        free_slot(regiments.alive, slot);
    }

    ArmyTotals army_totals(const Regiments& regiments, std::uint16_t army)
    {
        ArmyTotals out;
        for_each_alive(regiments.alive, [&](std::uint32_t r) {
            if (regiments.army[r] == army) {
                ++out.regiments;
                out.men += regiments.strength[r];
                out.morale += regiments.morale[r];
            }
        });
        if (out.regiments > 0) {
            out.morale /= static_cast<float>(out.regiments);
        }
        return out;
    }
}
