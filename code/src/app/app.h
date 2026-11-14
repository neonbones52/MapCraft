#pragma once

#include <filesystem>
#include <vector>
#include <string>

#include <d3d11.h>

#include "core/camera.h"
#include "app/map_modes.h"
#include "core/map_data.h"
#include "render/map_renderer.h"

namespace mc
{
    class GameData;

    // The map as the hosts see it: give it a device once, then a render target, time step and input
    // every frame. Knows nothing about windows.
    class App
    {
    public:
        // The map file, the map's biome table (data/common/biomes.csv) and the Minecraft client jar whose textures
        // the blocks wear (minecraft::find_jar).
        bool init(ID3D11Device* device, const std::filesystem::path& map_file, const std::filesystem::path& biomes_csv,
                  const std::filesystem::path& jar, std::string* error);
        // The game data the map modes show (owned elsewhere: World in the game, the editor's own copy), or null:
        // without it (or while it is not loaded) the regions stand in for countries. Must outlive the App's use of it.
        void set_game(const GameData* game);
        // Camera (intents only: app/bindings.h turns keys into them), picking under the cursor, then the map is drawn.
        // Knows no keys: map modes, leaving etc. are the caller's.
        void frame(ID3D11DeviceContext* context, ID3D11RenderTargetView* target, float view_width, float view_height,
                   float dt, const CameraInput& input);

        void reset_camera(float view_width, float view_height);
        // The whole map in view again, from the next frame (when the view size is known).
        void request_camera_reset() { camera_ready_ = false; }
        // Carries out a pending reset now (frame() does it first thing anyway).
        void ensure_camera(float view_width, float view_height);


        [[nodiscard]] Camera& camera() { return camera_; }
        [[nodiscard]] const MapData& map() const { return map_; }
        [[nodiscard]] const MapRenderer& renderer() const { return renderer_; }
        void set_map_mode(MapMode mode);
        [[nodiscard]] MapMode map_mode() const { return map_mode_; }
        // After the game data changed: colours and names are rebuilt on the next frame.
        void refresh_map_mode() { colors_dirty_ = true; }
        // Provinces drawn as selected (editor): one flag per province id, empty = none.
        void set_selection(std::vector<std::uint8_t> selected);
        [[nodiscard]] std::uint32_t hovered() const { return hovered_; }

    private:
        [[nodiscard]] std::uint32_t province_under(const CameraView& view, float screen_x, float screen_y) const;

        MapData map_;
        const GameData* game_ = nullptr;   // not owned
        std::vector<std::uint8_t> selection_;
        bool selection_dirty_ = false;
        LabelRaster label_raster_;
        LabelFont label_font_;
        std::vector<MapLabel> labels_;
        float province_names_ = 0.0f;   // 0: the mode's names show, 1: the provinces' names (faded over time)
        Camera camera_;
        MapRenderer renderer_;
        MapMode map_mode_ = MapMode::political;
        bool colors_dirty_ = true;
        std::vector<ProvinceFill> fills_;
        std::vector<BorderStyle> styles_;
        bool camera_ready_ = false;
        double time_ = 0.0;
        std::uint32_t hovered_ = 0;
    };
}
