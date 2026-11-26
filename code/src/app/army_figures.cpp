#include "app/army_figures.h"

#include <algorithm>
#include <cmath>

#include "game/armies.h"
#include "game/game_data.h"
#include "minecraft/entity_models.h"
#include "view/map_view.h"

namespace mc
{
    namespace
    {
        constexpr float kBlock = 6.0f;          // world units per Minecraft block of a figure (a house's: 1)
        constexpr float kSpacing = 1.6f;        // blocks between the figures of armies in one province
        constexpr float kFacingDegrees = 35.0f; // turned from the south (to the viewer) toward the east: a 3/4 view

        // A part's 3 x 3 turn as the game's PartPose: Rz * Ry * Rx.
        void rotation(const minecraft::PartRotation& r, float out[3][3])
        {
            const float cx = std::cos(r.x), sx = std::sin(r.x);
            const float cy = std::cos(r.y), sy = std::sin(r.y);
            const float cz = std::cos(r.z), sz = std::sin(r.z);
            const float rx[3][3] = { { 1, 0, 0 }, { 0, cx, -sx }, { 0, sx, cx } };
            const float ry[3][3] = { { cy, 0, sy }, { 0, 1, 0 }, { -sy, 0, cy } };
            const float rz[3][3] = { { cz, -sz, 0 }, { sz, cz, 0 }, { 0, 0, 1 } };
            float zy[3][3];
            for (int i = 0; i < 3; ++i) {
                for (int j = 0; j < 3; ++j) {
                    zy[i][j] = rz[i][0] * ry[0][j] + rz[i][1] * ry[1][j] + rz[i][2] * ry[2][j];
                }
            }
            for (int i = 0; i < 3; ++i) {
                for (int j = 0; j < 3; ++j) {
                    out[i][j] = zy[i][0] * rx[0][j] + zy[i][1] * rx[1][j] + zy[i][2] * rx[2][j];
                }
            }
        }

        // Model space (px: x the entity's left, y down, z back, the ground at y = 24) to the world: standing at
        // `ground`, facing `forward` (level), `scale` world units a model px.
        struct Placement
        {
            float a[3][3];   // the linear part, columns: model x, y, z
            Vec3 origin;     // where model (0, 0, 0) lands

            Placement(Vec3 ground, Vec3 forward, float scale)
            {
                const Vec3 up{ 0.0f, 1.0f, 0.0f };
                const Vec3 left = cross(forward, up);
                const Vec3 cols[3] = { left * scale, up * -scale, forward * -scale };
                for (int c = 0; c < 3; ++c) {
                    a[0][c] = cols[c].x;
                    a[1][c] = cols[c].y;
                    a[2][c] = cols[c].z;
                }
                origin = ground + up * (24.0f * scale);
            }
            [[nodiscard]] Vec3 point(const float p[3]) const
            {
                return { origin.x + a[0][0] * p[0] + a[0][1] * p[1] + a[0][2] * p[2],
                         origin.y + a[1][0] * p[0] + a[1][1] * p[1] + a[1][2] * p[2],
                         origin.z + a[2][0] * p[0] + a[2][1] * p[1] + a[2][2] * p[2] };
            }
            // The part's matrix: its pivot, then its turn.
            [[nodiscard]] FigureMatrix part(const std::array<float, 3>& pivot, const minecraft::PartRotation& turn) const
            {
                float r[3][3];
                rotation(turn, r);
                const Vec3 at = point(pivot.data());
                FigureMatrix m{};
                for (int i = 0; i < 3; ++i) {
                    for (int j = 0; j < 3; ++j) {
                        m.m[i][j] = a[i][0] * r[0][j] + a[i][1] * r[1][j] + a[i][2] * r[2][j];
                    }
                }
                m.m[0][3] = at.x;
                m.m[1][3] = at.y;
                m.m[2][3] = at.z;
                return m;
            }
        };

        void add_model(const minecraft::EntityModel& model, FigureSet* out)
        {
            FigureSet::Model m;
            m.first = static_cast<std::uint32_t>(out->vertices.size());
            m.count = static_cast<std::uint32_t>(model.vertices.size());
            for (const minecraft::EntityVertex& v : model.vertices) {
                FigureSet::Vertex fv{};
                std::copy(std::begin(v.pos), std::end(v.pos), fv.pos);
                std::copy(std::begin(v.normal), std::end(v.normal), fv.normal);
                std::copy(std::begin(v.uv), std::end(v.uv), fv.uv);
                fv.layer_part = std::uint32_t{ v.layer } | std::uint32_t{ v.part } << 16;
                out->vertices.push_back(fv);
            }
            out->models.push_back(m);
        }

        // The top of the ground at a map point, world height (as the camera's ground, app.cpp).
        float ground_at(const MapView& map, float map_x, float map_y)
        {
            const float size = static_cast<float>(map.block_size);
            const auto bx = static_cast<std::int64_t>(std::floor(map_x / size));
            const auto by = static_cast<std::int64_t>(std::floor(map_y / size));
            if (bx < 0 || by < 0 || bx >= map.block_width || by >= map.block_height) {
                return 0.0f;
            }
            const std::size_t i = static_cast<std::size_t>(by) * map.block_width + static_cast<std::size_t>(bx);
            return static_cast<float>(std::max(map.block_ground[i], map.block_water[i]) - 1) * size;
        }
    }

    bool ArmyFigures::init(const minecraft::Assets& assets, FigureSet* out, std::string* error)
    {
        *out = FigureSet{};
        minecraft::EntityTextures textures;
        minecraft::EntityModel soldier_model;
        minecraft::EntityModel horse_model;
        if (!minecraft::iron_soldier(assets, &textures, &soldier_model, error) ||
            !minecraft::iron_horse(assets, &textures, &horse_model, error)) {
            return false;
        }
        static_assert(FigureSet::kTextureSize == minecraft::EntityTextures::kSize);
        add_model(soldier_model, out);   // Model::soldier
        add_model(horse_model, out);     // Model::horse
        out->layers = std::move(textures.layers);
        out->layer_count = textures.count;
        pivots_[soldier] = soldier_model.pivots;
        pivots_[horse] = horse_model.pivots;
        return true;
    }

    void ArmyFigures::update(const MapView& map, const GameData* game, float time, const CameraView& camera)
    {
        draws_.clear();
        matrices_.clear();
        markers_.clear();
        if (game == nullptr || !game->loaded || pivots_[soldier].empty()) {
            return;
        }
        const Armies& armies = game->state.armies;
        const Regiments& regiments = game->state.regiments;
        const auto& category = game->content.regiment_category;

        // Men per army and category, then the live armies by province.
        constexpr std::size_t kCategories = 3;
        men_.assign(std::size_t{ kArmySlots } * kCategories, 0.0f);
        for_each_alive(regiments.alive, [&](std::uint32_t r) {
            const std::uint8_t type = regiments.type[r];
            const auto c = type < category.size() ? static_cast<std::size_t>(category[type]) : 0;
            men_[std::size_t{ regiments.army[r] } * kCategories + c] += regiments.strength[r];
        });
        order_.clear();
        for_each_alive(armies.alive, [&](std::uint32_t a) {
            if (armies.province[a] != 0 && armies.province[a] < map.count) {
                order_.push_back(a);
            }
        });
        std::ranges::stable_sort(order_, [&](std::uint32_t a, std::uint32_t b) {
            return armies.province[a] < armies.province[b];
        });

        const float facing = kFacingDegrees * 3.14159265f / 180.0f;
        const Vec3 forward{ std::sin(facing), 0.0f, -std::cos(facing) };
        const float px = kBlock / 16.0f;   // world units per model px
        std::array<minecraft::PartRotation, minecraft::villager_part_count> soldier_parts{};
        std::array<minecraft::PartRotation, minecraft::horse_part_count> horse_parts{};
        for (std::size_t i = 0; i < order_.size();) {
            const std::uint16_t province = armies.province[order_[i]];
            std::size_t end = i;
            while (end < order_.size() && armies.province[order_[end]] == province) {
                ++end;
            }
            const MapView::Point unit = map.unit_position(province);
            for (std::size_t k = i; k < end; ++k) {
                const std::uint32_t army = order_[k];
                const float shift = (static_cast<float>(k - i) - static_cast<float>(end - i - 1) * 0.5f) * kSpacing *
                                    kBlock;
                const float x = unit.x + shift;
                const Vec3 ground{ x, ground_at(map, x, unit.y), -unit.y };
                const float* m = &men_[std::size_t{ army } * kCategories];
                const bool mounted = m[static_cast<std::size_t>(RegimentCategory::cavalry)] >
                                         m[static_cast<std::size_t>(RegimentCategory::infantry)] &&
                                     m[static_cast<std::size_t>(RegimentCategory::cavalry)] >=
                                         m[static_cast<std::size_t>(RegimentCategory::artillery)];
                const float age = time * 20.0f + static_cast<float>(army) * 13.7f;   // each its own phase

                Placement rider(ground, forward, px);
                if (mounted) {
                    const Placement steed(ground, forward, px * minecraft::kHorseScale);
                    minecraft::horse_pose(age, &horse_parts);
                    draws_.push_back({ horse, static_cast<std::uint32_t>(matrices_.size()) });
                    for (std::size_t p = 0; p < horse_parts.size(); ++p) {
                        matrices_.push_back(steed.part(pivots_[horse][p], horse_parts[p]));
                    }
                    // The rider's ground point on the saddle.
                    const Vec3 seat = steed.point(minecraft::kHorseSeat);
                    rider = Placement(seat, forward, px);
                }
                minecraft::soldier_pose(age, mounted, &soldier_parts);
                draws_.push_back({ soldier, static_cast<std::uint32_t>(matrices_.size()) });
                for (std::size_t p = 0; p < soldier_parts.size(); ++p) {
                    matrices_.push_back(rider.part(pivots_[soldier][p], soldier_parts[p]));
                }

                // The plate under the feet, where the ground point is on the screen (in front of the camera).
                const Mat4& vp = camera.view_proj;
                const float cx = ground.x * vp.m[0][0] + ground.y * vp.m[1][0] + ground.z * vp.m[2][0] + vp.m[3][0];
                const float cy = ground.x * vp.m[0][1] + ground.y * vp.m[1][1] + ground.z * vp.m[2][1] + vp.m[3][1];
                const float cw = ground.x * vp.m[0][3] + ground.y * vp.m[1][3] + ground.z * vp.m[2][3] + vp.m[3][3];
                if (cw > 0.0f) {
                    const float sx = (cx / cw * 0.5f + 0.5f) * camera.view_width;
                    const float sy = (0.5f - cy / cw * 0.5f) * camera.view_height;
                    if (sx > -100.0f && sy > -100.0f && sx < camera.view_width + 100.0f &&
                        sy < camera.view_height + 100.0f) {
                        markers_.push_back({ static_cast<std::uint16_t>(army), sx, sy });
                    }
                }
            }
            i = end;
        }
    }
}
