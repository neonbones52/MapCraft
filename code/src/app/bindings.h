#pragma once

#include <optional>

#include "app/map_modes.h"
#include "view/camera.h"
#include "view/input.h"

namespace mc
{
    // Keys and buttons, in one place (EU4's layout). The game (Shell) and the editor turn their input into intents
    // here; the map (App, Camera) never sees a key code.

    // Right or middle drag, wheel, WASD / arrows, Home.
    [[nodiscard]] CameraInput camera_input(const Input& input);

    // 1 terrain, 2 political, 3 provinces, 4 regions.
    [[nodiscard]] std::optional<MapMode> map_mode_key(const Input& input);

    // What the keys ask of the game's pace this frame.
    struct SpeedInput
    {
        int step = 0;         // +1 faster, -1 slower
        bool pause = false;   // toggle the pause
    };
    // + / - (and the numpad's) change the speed, Space pauses.
    [[nodiscard]] SpeedInput speed_input(const Input& input);

    // The mouse as the player commands armies (app/army_orders.h), EU4's: the left button selects (a click, or a
    // drag that draws a box), the right orders the selected armies to the province clicked. The right button also
    // drags the camera (camera_input): a click is a press and a release without a drag.
    struct ArmyInput
    {
        float cursor_x = 0.0f;
        float cursor_y = 0.0f;
        bool select_pressed = false;
        bool select_released = false;
        bool order_pressed = false;
        bool order_released = false;
    };
    [[nodiscard]] ArmyInput army_input(const Input& input);

    // Esc: leave the game (close the window). The editor keeps Esc for its selection.
    [[nodiscard]] bool leave_key(const Input& input);
}
