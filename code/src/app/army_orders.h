#pragma once

#include <cstdint>
#include <span>
#include <vector>

#include "app/bindings.h"
#include "game/armies.h"
#include "game/commands.h"
#include "view/army_marker.h"

namespace mc
{
    class MapData;
    class World;

    // The player's command of their armies on the map, EU4's (the mouse as app/bindings.h's ArmyInput reads it):
    //   a left click on an own army's figure or plate selects it alone; anywhere else it clears the selection
    //   a left drag draws a box: on release the own armies whose figures it touches are the selection
    //   a right click on a land province orders every selected army there (a move_army command each)
    // Only the player's own armies are ever selected, never neutral or foreign ones; nothing is while the figures
    // are hidden. The selection is view state, army references (an army that dies or changes hands drops out); the
    // orders go out as commands (game/commands.h) for the Shell to post.
    class ArmyOrders
    {
    public:
        // This frame: the mouse (its presses already withheld where the UI has it), the map, the world and the
        // player, the armies on the screen, the army whose plate is under the cursor (its slot, or -1: the UI's) and
        // the province under it (0 none). Appends the orders given to `out`.
        void update(const ArmyInput& input, const MapData& map, const World& world, std::uint16_t player,
                    const ArmyMarkers& armies, int plate, std::uint32_t province, std::vector<Command>* out);
        // Nothing selected, no gesture under way (a new game).
        void clear();

        // The selected armies' slots, ascending.
        [[nodiscard]] std::span<const std::uint16_t> selected() const { return slots_; }
        // The box being dragged.
        [[nodiscard]] SelectionBox box() const { return box_; }

    private:
        std::vector<ArmyRef> selection_;
        std::vector<std::uint16_t> slots_;
        bool selecting_ = false;   // the left button went down on the map and is held
        float select_x_ = 0.0f;
        float select_y_ = 0.0f;
        bool ordering_ = false;    // the right button went down on the map and is held
        bool order_dragged_ = false;
        float order_x_ = 0.0f;
        float order_y_ = 0.0f;
        SelectionBox box_;
    };
}
