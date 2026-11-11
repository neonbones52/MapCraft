#include "app/app.h"

#include <algorithm>
#include <chrono>
#include <cmath>

#include "core/log.h"

namespace mc
{
    namespace
    {
        constexpr float kProvinceNamesLevel = 8.5f;   // wheel notches in from the whole map (level 1 = 0 notches)
        constexpr float kNamesFade = 0.25f;           // s for the cross-fade
        constexpr float kViewMargin = 32.0f;          // map px the farthest view shows round the map
    }

    bool App::init(ID3D11Device* device, const std::filesystem::path& map_file, std::string* error)
    {
        if (!map_.load(map_file, error)) {
            return false;
        }
        log::info("map {}: {}x{}, {} provinces, {} border chains, {} rivers", map_file.string(), map_.width,
                  map_.height, map_.count - 1, map_.border_chains.size() / 4, map_.river_chains.size() / 4);
        camera_.set_margin(kViewMargin);
        const auto started = std::chrono::steady_clock::now();
        label_raster_.build(map_);
        label_font_.build(map_);
        log::info("label raster: {} provinces with pixels, {:.0f} ms", label_raster_.used.size(),
                  std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - started).count());
        if (!renderer_.init(device, map_, error)) {
            return false;
        }
        // Province names are baked (tools/labels/provinces.py), one label per glyph run of the file.
        std::vector<MapLabel> provinces(1);
        for (std::size_t i = 0; i + 7 <= map_.province_glyphs.size(); i += 7) {
            const float* g = &map_.province_glyphs[i];
            provinces[0].glyphs.push_back({ g[0], g[1], g[2], g[3], g[4], g[5], static_cast<std::uint8_t>(g[6]) });
        }
        renderer_.set_province_labels(map_, label_font_, provinces);
        log::info("province names: {} glyphs", provinces[0].glyphs.size());
        return true;
    }

    void App::set_game(const GameData* game)
    {
        game_ = game;
        colors_dirty_ = true;
    }

    void App::set_selection(std::vector<std::uint8_t> selected)
    {
        selection_ = std::move(selected);
        selection_.resize(map_.count, 0);
        selection_dirty_ = true;
    }

    void App::reset_camera(float view_width, float view_height)
    {
        camera_.reset(static_cast<float>(map_.width), static_cast<float>(map_.height), view_width, view_height);
        camera_ready_ = true;
    }

    void App::ensure_camera(float view_width, float view_height)
    {
        if (!camera_ready_) {
            reset_camera(view_width, view_height);
        }
    }

    void App::set_map_mode(MapMode mode)
    {
        if (mode != map_mode_) {
            map_mode_ = mode;
            colors_dirty_ = true;
        }
    }

    std::uint32_t App::province_under(const CameraView& view, float screen_x, float screen_y) const
    {
        float mx = 0.0f;
        float my = 0.0f;
        if (!camera_.pick(view, screen_x, screen_y, &mx, &my)) {
            return 0;
        }
        if (!(mx >= 0.0f && my >= 0.0f && mx < static_cast<float>(map_.width) && my < static_cast<float>(map_.height))) {
            return 0;
        }
        const auto x = static_cast<std::size_t>(mx);
        const auto y = static_cast<std::size_t>(my);
        return map_.pixel_province[y * map_.width + x];
    }

    void App::frame(ID3D11DeviceContext* context, ID3D11RenderTargetView* target, float view_width, float view_height,
                    float dt, const CameraInput& input)
    {
        ensure_camera(view_width, view_height);
        if (colors_dirty_) {
            map_mode_colors(map_, game_, map_mode_, &fills_, &styles_);
            renderer_.set_colors(context, fills_, styles_);
            const auto started = std::chrono::steady_clock::now();
            labels_ = map_mode_labels(map_, game_, label_raster_, label_font_, map_mode_);
            renderer_.set_labels(map_, label_font_, labels_);
            log::info("map names: {} labels, {:.0f} ms", labels_.size(),
                      std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - started).count());
            colors_dirty_ = false;
        }
        if (selection_dirty_) {
            renderer_.set_selection(context, selection_);
            selection_dirty_ = false;
        }
        time_ += dt;
        camera_.update(input, dt, view_width, view_height);

        FrameView view;
        view.camera = camera_.view(view_width, view_height);
        hovered_ = province_under(view.camera, input.cursor_x, input.cursor_y);
        view.time = static_cast<float>(std::fmod(time_, 3600.0));
        view.hovered = hovered_;
        view.map_mode = static_cast<std::uint32_t>(map_mode_);
        // From the 10th zoom level (counted from the whole map as the 1st) the provinces' names replace the mode's
        // names: a short cross-fade once the zoom passes halfway between the 9th and the 10th.
        const bool provinces_shown = camera_.zoom_level(view_width, view_height) > kProvinceNamesLevel;
        const float step = dt / kNamesFade;
        province_names_ = std::clamp(province_names_ + (provinces_shown ? step : -step), 0.0f, 1.0f);
        view.mode_names_alpha = 1.0f - province_names_;
        view.province_names_alpha = province_names_;
        renderer_.draw(context, target, view);
    }
}
