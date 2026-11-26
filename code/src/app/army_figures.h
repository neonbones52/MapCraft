#pragma once

#include <array>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

#include "render/figures.h"
#include "view/army_marker.h"
#include "view/camera.h"

namespace mc
{
    class GameData;
    class MapView;
    class Movement;
    namespace minecraft
    {
        class Assets;
    }

    // How the game's time flows for the marching figures this frame (the Shell's Pace, view/pace.h).
    struct MarchClock
    {
        float day_fraction = 0.0f;   // of the coming day gone by (kept in the pause): a marching figure walks on by
                                     // it between days
        bool running = false;        // days pass (playing, not paused): marching figures walk, else they stand
    };

    // The armies on the map: a figure per live army at its province's unit position (several in a province stand side
    // by side, in slot order), and the point under each figure where its plate goes. A marching army's figure walks
    // the line from its province's unit position to the next's, as far as its step's progress (game/movement.h) and
    // the day's fraction take it, turned the way it goes, its legs (or its horse's) in the game's walk while days
    // pass; standing, it turns back to the viewer and its pose is the idle one. The figure is its men's plurality:
    // cavalry rides the armoured horse; infantry and, until it has a model of its own, artillery stand on foot (the
    // user's decision). Reads the state only; owned by App; keeps only how each figure moves (view state).
    class ArmyFigures
    {
    public:
        // The models from the jar (minecraft/entity_models.h) into the renderer's figure set.
        bool init(const minecraft::Assets& assets, FigureSet* out, std::string* error);
        // This frame: the live armies of `game` (null or not loaded: none), the march's cache (null: everyone
        // stands, as in the editor), the time's flow, the animation clock in s and the frame's dt, the camera.
        void update(const MapView& map, const GameData* game, const Movement* movement, MarchClock clock, float time,
                    float dt, const CameraView& camera);

        [[nodiscard]] std::span<const FigureDraw> draws() const { return draws_; }
        [[nodiscard]] std::span<const FigureMatrix> matrices() const { return matrices_; }
        [[nodiscard]] std::span<const ArmyMarker> markers() const { return markers_; }

    private:
        enum Model : std::uint32_t
        {
            soldier = 0,
            horse = 1,
        };
        std::array<std::vector<std::array<float, 3>>, 2> pivots_;   // per model, per part (model px)
        // How each army slot's figure moves (view state): the walk's position and speed (minecraft/entity_models.h),
        // its heading (radians from the south toward the east), the march it showed last frame (to tell an arrival
        // from a step given up) and where on a step's path it was shown, the walk back along a step given up, and
        // the army's generation they belong to.
        struct Motion
        {
            float walk = 0.0f;
            float amount = 0.0f;
            float yaw = 0.0f;
            std::uint16_t province = 0;
            std::uint16_t next = 0;
            std::uint16_t shown_from = 0;
            std::uint16_t shown_to = 0;   // 0: shown standing (or walking back)
            float shown_t = 0.0f;
            bool returning = false;       // walking back along the path from `province` to back_to, now at back_t
            std::uint16_t back_to = 0;
            float back_t = 0.0f;
            std::uint16_t generation = 0;
            bool seen = false;
        };
        std::vector<Motion> motion_;                               // per army slot
        std::vector<float> men_;                                   // scratch: per army slot, per category
        std::vector<std::uint32_t> order_;                         // scratch: live armies by province, then slot
        std::vector<FigureDraw> draws_;
        std::vector<FigureMatrix> matrices_;
        std::vector<ArmyMarker> markers_;
    };
}
