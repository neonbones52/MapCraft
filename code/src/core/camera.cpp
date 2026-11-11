#include "core/camera.h"

#include <algorithm>
#include <cmath>
#include <numbers>

namespace mc
{
    namespace
    {
        constexpr float kFovY = 30.0f * std::numbers::pi_v<float> / 180.0f;
        constexpr float kMaxPitch = 30.0f * std::numbers::pi_v<float> / 180.0f;  // at the closest zoom
        constexpr float kTiltStart = 0.5f;        // log2 scale where the camera starts to tilt
        constexpr float kMinLogScale = -2.5f;     // closest: ~5.7 screen px per map px
        constexpr float kZoomStep = 0.30f;        // log2 per wheel notch
        constexpr float kZoomSharpness = 14.0f;   // 1/s, how fast the zoom catches its target
        constexpr float kPanSpeed = 1100.0f;      // screen px per second at full keyboard speed
        constexpr float kPanAcceleration = 9.0f;  // 1/s toward the keyboard target velocity
        constexpr float kFriction = 4.5f;         // 1/s decay of coasting velocity
        constexpr float kDragSmoothing = 20.0f;   // 1/s, filter for the fling velocity
        constexpr float kMinFling = 30.0f;        // screen px per second below which a release does not fling
        constexpr float kFlightMin = 0.8f;        // s, shortest and longest flight (fly_to)
        constexpr float kFlightMax = 2.2f;

        float approach(float current, float target, float rate, float dt)
        {
            return target + (current - target) * std::exp(-rate * dt);
        }

        float smoothstep(float x)
        {
            x = std::clamp(x, 0.0f, 1.0f);
            return x * x * (3.0f - 2.0f * x);
        }

        float smootherstep(float x)
        {
            x = std::clamp(x, 0.0f, 1.0f);
            return x * x * x * (x * (6.0f * x - 15.0f) + 10.0f);
        }

        bool steers(const CameraInput& input)
        {
            return input.wheel != 0.0f || input.pan_x != 0.0f || input.pan_y != 0.0f || input.home || input.drag_start;
        }
    }

    Vec3 CameraView::ray(float screen_x, float screen_y) const
    {
        const float nx = 2.0f * screen_x / view_width - 1.0f;
        const float ny = 1.0f - 2.0f * screen_y / view_height;
        return normalize(forward + right * (nx * tan_half_fov * aspect) + up * (ny * tan_half_fov));
    }

    bool CameraView::ground(float screen_x, float screen_y, float plane, float* map_x, float* map_y) const
    {
        const Vec3 d = ray(screen_x, screen_y);
        if (d.y > -1e-6f) {
            return false;
        }
        const Vec3 p = eye + d * ((plane - eye.y) / d.y);
        *map_x = p.x;
        *map_y = -p.z;
        return true;
    }

    void CameraView::frustum(float planes[6][4]) const
    {
        const auto& m = view_proj.m;
        for (int i = 0; i < 4; ++i) {
            const float c0 = m[i][0], c1 = m[i][1], c2 = m[i][2], c3 = m[i][3];
            planes[0][i] = c3 + c0;
            planes[1][i] = c3 - c0;
            planes[2][i] = c3 + c1;
            planes[3][i] = c3 - c1;
            planes[4][i] = c2;
            planes[5][i] = c3 - c2;
        }
    }

    bool box_visible(const float planes[6][4], const float lo[3], const float hi[3])
    {
        for (int p = 0; p < 6; ++p) {
            const float x = planes[p][0] >= 0.0f ? hi[0] : lo[0];
            const float y = planes[p][1] >= 0.0f ? hi[1] : lo[1];
            const float z = planes[p][2] >= 0.0f ? hi[2] : lo[2];
            if (planes[p][0] * x + planes[p][1] * y + planes[p][2] * z + planes[p][3] < 0.0f) {
                return false;
            }
        }
        return true;
    }

    float Camera::scale() const
    {
        return std::exp2(log_scale_);
    }

    CameraView Camera::view(float view_width, float view_height) const
    {
        CameraView v;
        v.view_width = std::max(view_width, 1.0f);
        v.view_height = std::max(view_height, 1.0f);
        v.aspect = v.view_width / v.view_height;
        v.tan_half_fov = std::tan(kFovY * 0.5f);
        v.pixel_angle = 2.0f * v.tan_half_fov / v.view_height;
        v.scale = scale();
        const float distance = v.scale / v.pixel_angle;
        const float pitch = kMaxPitch * smoothstep((kTiltStart - log_scale_) / (kTiltStart - kMinLogScale));
        const Vec3 target{ x_, 0.0f, -y_ };
        v.eye = target + Vec3{ 0.0f, std::cos(pitch), -std::sin(pitch) } * distance;
        v.forward = normalize(target - v.eye);
        v.right = normalize(cross(Vec3{ 0.0f, 0.0f, 1.0f }, v.forward));
        v.up = cross(v.forward, v.right);
        const Mat4 view = look_at_lh(v.eye, target, Vec3{ 0.0f, 0.0f, 1.0f });
        const Mat4 proj = perspective_lh(kFovY, v.aspect, distance * 0.02f, distance * 4.0f + 200.0f);
        v.view_proj = view * proj;
        return v;
    }

    float Camera::zoom_level(float view_width, float view_height) const
    {
        return (max_log_scale(view_width, view_height) - log_scale_) / kZoomStep;
    }

    float Camera::max_log_scale(float view_width, float view_height) const
    {
        const float fit = std::max((map_w_ + 2.0f * margin_) / std::max(view_width, 1.0f),
                                   (map_h_ + 2.0f * margin_) / std::max(view_height, 1.0f));
        return std::log2(fit);
    }

    void Camera::keep_in_bounds(float view_width, float view_height)
    {
        // What the screen edges see on the sea-level plane, relative to the centre. The far (top) corners are the
        // widest when the camera tilts.
        const CameraView v = view(view_width, view_height);
        float lx = 0.0f, ly = 0.0f, rx = 0.0f, ry = 0.0f, tx = 0.0f, ty = 0.0f, bx = 0.0f, by = 0.0f;
        if (!v.ground(0.0f, 0.0f, 0.0f, &lx, &ly) || !v.ground(view_width, 0.0f, 0.0f, &rx, &ry) ||
            !v.ground(view_width * 0.5f, 0.0f, 0.0f, &tx, &ty) ||
            !v.ground(view_width * 0.5f, view_height, 0.0f, &bx, &by)) {
            return;
        }
        const float left = x_ - lx;
        const float right = rx - x_;
        const float top = y_ - ty;
        const float bottom = by - y_;
        auto clamp_axis = [](float c, float lo, float hi, float before, float after) {
            const float min_c = lo + before;
            const float max_c = hi - after;
            // The view is at least as wide as the bounds: centred, no panning along this axis.
            return min_c >= max_c ? (lo + hi + before - after) * 0.5f : std::clamp(c, min_c, max_c);
        };
        const float cx = clamp_axis(x_, -margin_, map_w_ + margin_, left, right);
        const float cy = clamp_axis(y_, -margin_, map_h_ + margin_, top, bottom);
        if (cx != x_) {
            vx_ = 0.0f;
        }
        if (cy != y_) {
            vy_ = 0.0f;
        }
        if (dragging_) {
            grab_x_ += cx - x_;
            grab_y_ += cy - y_;
        }
        x_ = cx;
        y_ = cy;
    }

    float Camera::ground_height(float map_x, float map_y) const
    {
        return height_at_ ? height_at_(map_x, map_y) : 0.0f;
    }

    bool Camera::pick(const CameraView& view, float screen_x, float screen_y, float* map_x, float* map_y,
                      float* plane) const
    {
        // Ray against the terrain: start on the sea-level plane, then re-intersect at the height found there.
        float h = 0.0f;
        if (!view.ground(screen_x, screen_y, h, map_x, map_y)) {
            return false;
        }
        for (int i = 0; i < 4; ++i) {
            h = std::max(ground_height(*map_x, *map_y), 0.0f);
            view.ground(screen_x, screen_y, h, map_x, map_y);
        }
        if (plane) {
            *plane = h;
        }
        return true;
    }

    void Camera::reset(float map_width, float map_height, float view_width, float view_height)
    {
        map_w_ = map_width;
        map_h_ = map_height;
        x_ = map_width * 0.5f;
        y_ = map_height * 0.5f;
        log_scale_ = target_log_scale_ = max_log_scale(view_width, view_height);
        vx_ = vy_ = drag_vx_ = drag_vy_ = 0.0f;
        dragging_ = false;
        flight_.active = false;
    }

    void Camera::set_view(float x, float y, float scale)
    {
        x_ = x;
        y_ = y;
        log_scale_ = target_log_scale_ = std::log2(scale);
        vx_ = vy_ = 0.0f;
        flight_.active = false;
    }

    float Camera::scale_at_level_from_closest(int level)
    {
        return std::exp2(kMinLogScale + static_cast<float>(std::max(level, 1) - 1) * kZoomStep);
    }

    void Camera::fly_to(float x, float y, float scale, float view_width, float view_height)
    {
        Flight& f = flight_;
        f.from_x = x_;
        f.from_y = y_;
        f.from_log = log_scale_;
        f.to_x = x;
        f.to_y = y;
        f.to_log = std::clamp(std::log2(scale), kMinLogScale, max_log_scale(view_width, view_height));
        // Longer for a deeper zoom and for a destination farther across the screen.
        const float across = std::hypot(x - x_, y - y_) / this->scale() / std::hypot(view_width, view_height);
        f.duration = std::clamp(0.7f + 0.22f * std::abs(f.to_log - f.from_log) + 0.25f * std::min(across, 2.0f),
                                kFlightMin, kFlightMax);
        f.time = 0.0f;
        f.active = true;
        vx_ = vy_ = drag_vx_ = drag_vy_ = 0.0f;
        dragging_ = false;
    }

    void Camera::update(const CameraInput& input, float dt, float view_width, float view_height)
    {
        dt = std::clamp(dt, 0.0f, 0.1f);
        const float max_log = max_log_scale(view_width, view_height);

        if (flight_.active && steers(input)) {
            flight_.active = false;
            target_log_scale_ = log_scale_;
        }
        if (input.home) {
            reset(map_w_, map_h_, view_width, view_height);
            return;
        }
        if (flight_.active) {
            // The destination keeps its place on the screen scaled by (1 - e): it slides to the centre as the zoom
            // closes in, the way a wheel zoom anchored on it would.
            Flight& f = flight_;
            f.time += dt;
            const float e = smootherstep(f.time / f.duration);
            log_scale_ = target_log_scale_ = std::min(f.from_log + (f.to_log - f.from_log) * e, max_log);
            const float k = (1.0f - e) * std::exp2(log_scale_ - f.from_log);
            x_ = f.to_x - k * (f.to_x - f.from_x);
            y_ = f.to_y - k * (f.to_y - f.from_y);
            if (f.time >= f.duration) {
                f.active = false;
            }
            keep_in_bounds(view_width, view_height);
            return;
        }

        // Zoom target: wheel notches (log scale falls as the camera closes in).
        target_log_scale_ -= input.wheel * kZoomStep;
        target_log_scale_ = std::clamp(target_log_scale_, kMinLogScale, max_log);

        // Smoothed zoom anchored at the cursor: the ground point under it stays put.
        const bool cursor_inside = input.cursor_x >= 0.0f && input.cursor_y >= 0.0f &&
                                   input.cursor_x < view_width && input.cursor_y < view_height;
        float anchor_x = 0.0f;
        float anchor_y = 0.0f;
        float anchor_plane = 0.0f;
        const bool anchored = cursor_inside &&
                              pick(view(view_width, view_height), input.cursor_x, input.cursor_y, &anchor_x, &anchor_y,
                                   &anchor_plane);
        log_scale_ = std::min(approach(log_scale_, target_log_scale_, kZoomSharpness, dt), max_log);
        if (anchored) {
            float now_x = 0.0f;
            float now_y = 0.0f;
            if (view(view_width, view_height).ground(input.cursor_x, input.cursor_y, anchor_plane, &now_x, &now_y)) {
                x_ += anchor_x - now_x;
                y_ += anchor_y - now_y;
            }
        }
        const float new_scale = scale();

        // Drag: the grabbed ground point follows the cursor exactly.
        if (!dragging_ && input.drag_start && cursor_inside &&
            pick(view(view_width, view_height), input.cursor_x, input.cursor_y, &grab_x_, &grab_y_, &grab_plane_)) {
            dragging_ = true;
            vx_ = vy_ = drag_vx_ = drag_vy_ = 0.0f;
        }
        if (dragging_) {
            const float prev_x = x_;
            const float prev_y = y_;
            float now_x = 0.0f;
            float now_y = 0.0f;
            if (view(view_width, view_height).ground(input.cursor_x, input.cursor_y, grab_plane_, &now_x, &now_y)) {
                x_ += grab_x_ - now_x;
                y_ += grab_y_ - now_y;
            }
            if (dt > 0.0f) {
                drag_vx_ = approach(drag_vx_, (x_ - prev_x) / dt, kDragSmoothing, dt);
                drag_vy_ = approach(drag_vy_, (y_ - prev_y) / dt, kDragSmoothing, dt);
            }
            if (!input.dragging) {
                dragging_ = false;
                const float speed = std::hypot(drag_vx_, drag_vy_) / new_scale;
                if (speed > kMinFling) {
                    vx_ = drag_vx_;
                    vy_ = drag_vy_;
                }
            }
        } else {
            // Held panning accelerates toward a target velocity; without it the camera coasts.
            const float dir_x = input.pan_x;
            const float dir_y = input.pan_y;
            if (dir_x != 0.0f || dir_y != 0.0f) {
                const float norm = 1.0f / std::hypot(dir_x, dir_y);
                const float speed = kPanSpeed * new_scale;
                vx_ = approach(vx_, dir_x * norm * speed, kPanAcceleration, dt);
                vy_ = approach(vy_, dir_y * norm * speed, kPanAcceleration, dt);
            } else {
                vx_ = approach(vx_, 0.0f, kFriction, dt);
                vy_ = approach(vy_, 0.0f, kFriction, dt);
            }
            x_ += vx_ * dt;
            y_ += vy_ * dt;
        }

        // Keep the view inside the map and its frame; stop coasting into the edge.
        keep_in_bounds(view_width, view_height);
    }
}
