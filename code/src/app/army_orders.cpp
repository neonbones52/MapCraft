#include "app/army_orders.h"

#include <algorithm>
#include <cmath>
#include <limits>

#include "core/map_data.h"
#include "game/world.h"

namespace mc
{
    namespace
    {
        // Screen px the cursor moves before a press becomes a drag (a box, or the camera's pan).
        constexpr float kDrag = 5.0f;

        bool dragged(float x0, float y0, float x1, float y1)
        {
            return std::fabs(x1 - x0) > kDrag || std::fabs(y1 - y0) > kDrag;
        }

        bool in_figure(const ArmyMarker& m, float x, float y)
        {
            return x >= m.x - m.half_width && x <= m.x + m.half_width && y >= m.top && y <= m.bottom;
        }

        bool touches(const SelectionBox& box, const ArmyMarker& m)
        {
            const float lo_x = std::min(box.x0, box.x1);
            const float hi_x = std::max(box.x0, box.x1);
            const float lo_y = std::min(box.y0, box.y1);
            const float hi_y = std::max(box.y0, box.y1);
            return m.x + m.half_width >= lo_x && m.x - m.half_width <= hi_x && m.bottom >= lo_y && m.top <= hi_y;
        }
    }

    void ArmyOrders::clear()
    {
        selection_.clear();
        slots_.clear();
        selecting_ = false;
        ordering_ = false;
        box_ = {};
    }

    void ArmyOrders::update(const ArmyInput& input, const MapData& map, const World& world, std::uint16_t player,
                            const ArmyMarkers& armies, int plate, std::uint32_t province, std::vector<Command>* out)
    {
        const Armies& state = world.state().armies;
        auto own = [&](std::uint16_t slot) {
            return player != 0 && slot_alive(state.alive, slot) && state.owner[slot] == player;
        };
        std::erase_if(selection_, [&](ArmyRef ref) {
            std::uint16_t slot = 0;
            return !resolve(state, ref, &slot) || !own(slot);
        });
        const bool shown = armies.fade > 0.0f;

        // Selecting: a click picks the army under the cursor (its plate first, else the front-most figure), a drag
        // the box.
        if (input.select_pressed) {
            selecting_ = true;
            select_x_ = input.cursor_x;
            select_y_ = input.cursor_y;
        }
        if (selecting_) {
            box_ = { dragged(select_x_, select_y_, input.cursor_x, input.cursor_y), select_x_, select_y_,
                     input.cursor_x, input.cursor_y };
        }
        if (input.select_released && selecting_) {
            selecting_ = false;
            selection_.clear();
            if (shown && box_.shown) {
                for (const ArmyMarker& m : armies.markers) {
                    if (own(m.army) && touches(box_, m)) {
                        selection_.push_back(army_ref(state, m.army));
                    }
                }
            } else if (shown) {
                int picked = plate;
                if (picked < 0) {
                    float front = -std::numeric_limits<float>::infinity();   // the nearest figure: lowest feet
                    for (const ArmyMarker& m : armies.markers) {
                        if (in_figure(m, input.cursor_x, input.cursor_y) && m.bottom > front) {
                            front = m.bottom;
                            picked = m.army;
                        }
                    }
                }
                if (picked >= 0 && own(static_cast<std::uint16_t>(picked))) {
                    selection_.push_back(army_ref(state, static_cast<std::uint16_t>(picked)));
                }
            }
            box_ = {};
        }

        // Ordering: a right click (not the camera's drag) on a land province.
        if (input.order_pressed) {
            ordering_ = true;
            order_dragged_ = false;
            order_x_ = input.cursor_x;
            order_y_ = input.cursor_y;
        }
        if (ordering_ && dragged(order_x_, order_y_, input.cursor_x, input.cursor_y)) {
            order_dragged_ = true;
        }
        if (input.order_released && ordering_) {
            ordering_ = false;
            if (!order_dragged_ && province != 0 && province < map.count && map.kind[province] == ProvinceKind::land) {
                for (const ArmyRef ref : selection_) {
                    out->push_back(
                        Command{ CommandKind::move_army, player, ref, static_cast<std::uint16_t>(province) });
                }
            }
        }

        slots_.clear();
        for (const ArmyRef ref : selection_) {
            slots_.push_back(ref.slot);
        }
        std::ranges::sort(slots_);
    }
}
