#include "app/bindings.h"

namespace mc
{
    CameraInput camera_input(const Input& input)
    {
        constexpr std::uint32_t drag = mouse_right | mouse_middle;
        auto held = [&](std::uint8_t a, std::uint8_t b) { return input.held(a) || input.held(b) ? 1.0f : 0.0f; };
        CameraInput c;
        c.cursor_x = input.cursor_x;
        c.cursor_y = input.cursor_y;
        c.wheel = input.wheel;
        c.pan_x = held(key::d, key::right) - held(key::a, key::left);
        c.pan_y = held(key::s, key::down) - held(key::w, key::up);
        c.home = input.hit(key::home);
        c.drag_start = (input.pressed & drag) != 0;
        c.dragging = (input.buttons & drag) != 0;
        return c;
    }

    std::optional<MapMode> map_mode_key(const Input& input)
    {
        if (input.hit(key::digit1)) {
            return MapMode::terrain;
        }
        if (input.hit(key::digit2)) {
            return MapMode::political;
        }
        if (input.hit(key::digit3)) {
            return MapMode::provinces;
        }
        if (input.hit(key::digit4)) {
            return MapMode::regions;
        }
        return std::nullopt;
    }

    SpeedInput speed_input(const Input& input)
    {
        SpeedInput s;
        s.step = (input.hit(key::equals) || input.hit(key::numpad_plus) ? 1 : 0) -
                 (input.hit(key::minus) || input.hit(key::numpad_minus) ? 1 : 0);
        s.pause = input.hit(key::space);
        return s;
    }

    bool leave_key(const Input& input)
    {
        return input.hit(key::escape);
    }
}
