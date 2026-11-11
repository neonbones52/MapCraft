#pragma once

#include <functional>

#include "core/math.h"

namespace mc
{
    // What the renderer and picking need from the camera for one frame. World space: x = map x, y = height,
    // z = -map y (north is +z), one unit = one map pixel.
    struct CameraView
    {
        Mat4 view_proj;
        Vec3 eye;
        Vec3 right;
        Vec3 up;
        Vec3 forward;
        float tan_half_fov = 0.0f;
        float aspect = 1.0f;
        float view_width = 1.0f;
        float view_height = 1.0f;
        float pixel_angle = 0.0f;  // world size of one screen pixel at distance 1
        float scale = 1.0f;        // map px per screen px at the look-at point

        [[nodiscard]] Vec3 ray(float screen_x, float screen_y) const;
        // Map point where the ray through a screen pixel meets the plane at world height `plane`.
        bool ground(float screen_x, float screen_y, float plane, float* map_x, float* map_y) const;
        // Frustum planes (a, b, c, d; inside where a x + b y + c z + d >= 0), Gribb & Hartmann, D3D depth 0..1.
        void frustum(float planes[6][4]) const;
    };

    // What the camera is asked to do this frame: intents, not keys or buttons (those are bound in app/bindings.h).
    struct CameraInput
    {
        float cursor_x = -1.0f;   // screen px; negative = not over the map (no zoom anchor, no drag start)
        float cursor_y = -1.0f;
        float wheel = 0.0f;       // notches this frame, positive = in
        float pan_x = 0.0f;       // held pan direction: east positive
        float pan_y = 0.0f;       // south positive
        bool home = false;        // back to the whole map
        bool drag_start = false;  // grab the ground under the cursor
        bool dragging = false;    // keep holding it (released = fling)
    };

    // Whether a world-space box touches the frustum (conservative).
    [[nodiscard]] bool box_visible(const float planes[6][4], const float lo[3], const float hi[3]);

    // Paradox-style strategic camera: looks down, never rotates; tilts a little toward the horizon when close.
    // `scale` is map pixels per screen pixel at the look-at point. Zoom is smoothed in log space and anchored at the
    // cursor; panning has inertia (keyboard acceleration, drag fling, friction). Dragging keeps the grabbed ground
    // point exactly under the cursor.
    class Camera
    {
    public:
        using HeightAt = std::function<float(float map_x, float map_y)>;  // world units

        void reset(float map_width, float map_height, float view_width, float view_height);
        void update(const CameraInput& input, float dt, float view_width, float view_height);
        void set_ground(HeightAt height_at) { height_at_ = std::move(height_at); }
        // How far beyond the map edges the view may reach (the frame around the map), map px. The farthest zoom
        // shows exactly the map plus this margin; the view never shows anything further out.
        void set_margin(float margin) { margin_ = margin; }

        [[nodiscard]] float center_x() const { return x_; }
        [[nodiscard]] float center_y() const { return y_; }
        [[nodiscard]] float scale() const;
        [[nodiscard]] CameraView view(float view_width, float view_height) const;
        // Wheel notches in from the farthest zoom (0 = the whole map), fractional while zooming.
        [[nodiscard]] float zoom_level(float view_width, float view_height) const;

        void set_view(float x, float y, float scale);
        // Flies to look at map point (x, y) with `scale` map px per screen px: zoom and position eased together
        // with the destination held at a screen point that slides to the centre (zooming in "into" it). Any camera
        // input (wheel, drag, pan keys, Home) ends the flight where it is.
        void fly_to(float x, float y, float scale, float view_width, float view_height);
        [[nodiscard]] bool flying() const { return flight_.active; }
        // Map px per screen px at a zoom level counted from the closest (1 = the closest; one level = a wheel notch).
        [[nodiscard]] static float scale_at_level_from_closest(int level);

        // The terrain point under a screen pixel (map px); `plane` receives its world height.
        bool pick(const CameraView& view, float screen_x, float screen_y, float* map_x, float* map_y,
                  float* plane = nullptr) const;

    private:
        [[nodiscard]] float max_log_scale(float view_width, float view_height) const;
        [[nodiscard]] float ground_height(float map_x, float map_y) const;
        void keep_in_bounds(float view_width, float view_height);

        HeightAt height_at_;
        float map_w_ = 1.0f;
        float map_h_ = 1.0f;
        float margin_ = 0.0f;
        float x_ = 0.0f;
        float y_ = 0.0f;
        float log_scale_ = 0.0f;         // log2(map px per screen px)
        float target_log_scale_ = 0.0f;
        float vx_ = 0.0f;                // map px / s
        float vy_ = 0.0f;
        bool dragging_ = false;
        float grab_x_ = 0.0f;            // map point held under the cursor while dragging
        float grab_y_ = 0.0f;
        float grab_plane_ = 0.0f;        // its world height
        float drag_vx_ = 0.0f;           // smoothed drag velocity, becomes the fling on release
        float drag_vy_ = 0.0f;
        struct Flight
        {
            bool active = false;
            float time = 0.0f;
            float duration = 1.0f;
            float from_x = 0.0f, from_y = 0.0f, from_log = 0.0f;
            float to_x = 0.0f, to_y = 0.0f, to_log = 0.0f;
        } flight_;
    };
}
