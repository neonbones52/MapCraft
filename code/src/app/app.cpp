#include "app/app.h"

#include <algorithm>
#include <chrono>
#include <cmath>

#include "app/terrain_look.h"
#include "core/log.h"
#include "minecraft/assets.h"

namespace mc
{
    namespace
    {
        constexpr float kProvinceNamesLevel = 8.5f;   // wheel notches in from the whole map (level 1 = 0 notches)
        constexpr float kNamesFade = 0.25f;           // s for the cross-fade
        constexpr float kViewMargin = 32.0f;          // map px the farthest view shows round the map
    }

    bool App::init(ID3D11Device* device, const std::filesystem::path& map_file, const std::filesystem::path& biomes_csv,
                   const std::filesystem::path& jar, std::string* error)
    {
        if (!map_.load(map_file, error)) {
            return false;
        }
        if (jar.empty()) {
            std::string where;
            for (const std::filesystem::path& path : minecraft::jar_candidates()) {
                where += "\n" + path.string();
            }
            *error = "Minecraft 26.2 not found. Looked for its client jar at:" + where;
            return false;
        }
        minecraft::Assets assets;
        TerrainLook look;
        const auto look_started = std::chrono::steady_clock::now();
        if (!assets.open(jar, error) || !build_terrain_look(map_, biomes_csv, assets, &look, error)) {
            return false;
        }
        log::info("blocks: textures from {}, {} layers, {} materials, {:.0f} ms", jar.string(), look.layer_count,
                  look.materials.size(),
                  std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - look_started).count());
        // The camera picks and zooms on the top of the blocks (water or ground).
        camera_.set_ground([this](float map_x, float map_y) {
            const float size = static_cast<float>(map_.block_size);
            const float bx = std::floor(map_x / size);
            const float by = std::floor(map_y / size);
            if (!(bx >= 0.0f && by >= 0.0f && bx < static_cast<float>(map_.block_width) &&
                  by < static_cast<float>(map_.block_height))) {
                return 0.0f;
            }
            const std::size_t i = static_cast<std::size_t>(by) * map_.block_width + static_cast<std::size_t>(bx);
            const int top = std::max(map_.block_ground[i], map_.block_water[i]);
            return static_cast<float>(top - 1) * size;
        });
        log::info("map {}: {}x{}, {} provinces, {} border chains, {} rivers", map_file.string(), map_.width,
                  map_.height, map_.count - 1, map_.border_chains.size() / 4, map_.river_chains.size() / 4);
        camera_.set_margin(kViewMargin);
        const auto started = std::chrono::steady_clock::now();
        label_raster_.build(map_);
        label_font_.build(map_);
        log::info("label raster: {} provinces with pixels, {:.0f} ms", label_raster_.used.size(),
                  std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - started).count());
        if (!renderer_.init(device, map_, look, error)) {
            return false;
        }
        log::info("blocks: {} face runs", renderer_.terrain_quads());
        // Province names are baked (tools/labels/provinces.py): one label per province, so each lies on its own plane.
        std::vector<MapLabel> provinces(map_.count);
        for (std::size_t k = 0; k < map_.province_glyph_owner.size(); ++k) {
            const float* g = &map_.province_glyphs[k * 7];
            const std::uint16_t owner = map_.province_glyph_owner[k];
            if (owner < provinces.size()) {
                provinces[owner].glyphs.push_back(
                    { g[0], g[1], g[2], g[3], g[4], g[5], static_cast<std::uint8_t>(g[6]) });
            }
        }
        std::erase_if(provinces, [](const MapLabel& label) { return label.glyphs.empty(); });
        std::vector<std::uint32_t> own(map_.count);   // a province's name is grouped by the province itself
        for (std::uint32_t id = 0; id < map_.count; ++id) {
            own[id] = id;
        }
        renderer_.set_province_labels(map_, label_font_, provinces, own);
        log::info("province names: {} names, {} glyphs", provinces.size(), map_.province_glyph_owner.size());
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
            renderer_.set_labels(map_, label_font_, labels_, map_mode_label_keys(map_, game_, map_mode_));
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
