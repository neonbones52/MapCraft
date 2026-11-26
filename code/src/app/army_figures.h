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
    namespace minecraft
    {
        class Assets;
    }

    // The armies on the map: a figure per live army at its province's unit position (several in a province stand side
    // by side, in slot order), posed each frame at rest (their idle movement), and the point under each figure where
    // its plate goes. The figure is its men's plurality: cavalry rides the armoured horse; infantry and, until it has a
    // model of its own, artillery stand on foot (the user's decision). Reads the state only; owned by App.
    class ArmyFigures
    {
    public:
        // The models from the jar (minecraft/entity_models.h) into the renderer's figure set.
        bool init(const minecraft::Assets& assets, FigureSet* out, std::string* error);
        // This frame: the live armies of `game` (null or not loaded: none), the animation clock in s, the camera.
        void update(const MapView& map, const GameData* game, float time, const CameraView& camera);

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
        std::vector<float> men_;                                   // scratch: per army slot, per category
        std::vector<std::uint32_t> order_;                         // scratch: live armies by province, then slot
        std::vector<FigureDraw> draws_;
        std::vector<FigureMatrix> matrices_;
        std::vector<ArmyMarker> markers_;
    };
}
