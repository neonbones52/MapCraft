#include "app/army_figures.h"

#include <algorithm>
#include <cmath>

#include "game/armies.h"
#include "game/game_data.h"
#include "game/movement.h"
#include "minecraft/entity_models.h"
#include "view/map_view.h"

namespace mc
{
    namespace
    {
        constexpr float kBlock = 6.0f;          // world units per Minecraft block of a figure (a house's: 1)
        constexpr float kSpacing = 1.6f;        // blocks between the figures of armies in one province
        constexpr float kFacingDegrees = 35.0f; // turned from the south (to the viewer) toward the east: a 3/4 view
        constexpr float kStandingHeight = 2.1f; // blocks: the soldier in his helmet
        constexpr float kMountedHeight = 2.9f;  // and on the horse
        constexpr float kPlateDrop = 0.12f;     // of the figure's screen height: the plate's top below the ground point
        // Half the figure's box (picking, app/army_orders.h), of its screen height.
        constexpr float kStandingHalfWidth = 0.3f;
        constexpr float kMountedHalfWidth = 0.45f;
        constexpr float kPi = 3.14159265f;
        constexpr float kTicks = 20.0f;         // the game's ticks a second: the clock of its animations
        // The walk's speed while marching (WalkAnimationState's, 1 at full speed): a soldier's march, a horse's walk.
        constexpr float kSoldierWalk = 0.7f;
        constexpr float kHorseWalk = 0.6f;
        constexpr float kWalkEase = 0.4f;       // of the way to the speed wanted, a tick
        constexpr float kWalkCycle = 1000.0f * 2.0f * kPi / 0.6662f;   // the walk's position wraps on whole strides
        constexpr float kTurnRate = 6.0f;       // radians a second a figure turns toward where it goes
        // A figure walking back along the path of a step given up: map px a second (faster than a march looks at
        // speed 4).
        constexpr float kReturnSpeed = 40.0f;
        // A battle (the user's decision): the attacker's figure on the left (west) of the unit position, the
        // defender's on the right, facing each other this many blocks apart, centre to centre, on foot and mounted
        // alike (the horses' heads just clear); the province's other armies stand in the row on from the defender's.
        constexpr float kBattleGap = 3.0f;
        // A sword's swing every 20 / 1.6 ticks (a full blow at the sword's attack speed), give or take this share.
        constexpr float kSwingEvery = kTicks / 1.6f;
        constexpr float kSwingSpread = 0.15f;
        // Where an army of a province stands (ArmyFigures::place_): its place in the row, or one of these.
        constexpr std::int32_t kAttacker = -1;
        constexpr std::int32_t kDefender = -2;
        constexpr std::int32_t kUnseen = -3;   // a side's army after its first: not shown in the battle

        // 0..1, a hash of an army slot and a count: each swing's spread (view only).
        float spread(std::uint32_t army, std::uint32_t n)
        {
            std::uint32_t h = army * 0x9E3779B1u ^ (n + 1u) * 0x85EBCA77u;
            h ^= h >> 15;
            h *= 0x2C1B3C6Du;
            h ^= h >> 12;
            h *= 0x297A2D39u;
            h ^= h >> 15;
            return static_cast<float>(h >> 8) * (1.0f / 16777216.0f);
        }

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

        // The point at `t` (0..1, by length) of the march path from province `from` to its neighbour `to`, and in
        // `yaw` the way the path heads there (radians from the south toward the east; map y grows to the south).
        // Without a path (not a step of the map): the straight line between their unit points.
        MapView::Point along(const MapView& map, std::uint16_t from, std::uint16_t to, float t, float* yaw)
        {
            const int entry = map.adjacency_entry(from, to);
            const std::span<const float> path = map.march_path(entry);
            if (path.size() < 4) {
                const MapView::Point a = map.unit_position(from);
                const MapView::Point b = map.unit_position(to);
                if (a.x != b.x || a.y != b.y) {
                    *yaw = std::atan2(b.x - a.x, b.y - a.y);
                }
                return { a.x + (b.x - a.x) * t, a.y + (b.y - a.y) * t };
            }
            float left = t * map.march_length[static_cast<std::size_t>(entry)];
            const std::size_t points = path.size() / 2;
            for (std::size_t i = 0; i + 1 < points; ++i) {
                const float ax = path[i * 2], ay = path[i * 2 + 1];
                const float dx = path[i * 2 + 2] - ax, dy = path[i * 2 + 3] - ay;
                const float piece = std::sqrt(dx * dx + dy * dy);
                if (piece <= 0.0f) {
                    continue;
                }
                *yaw = std::atan2(dx, dy);
                if (left <= piece || i + 2 == points) {
                    const float f = std::clamp(left / piece, 0.0f, 1.0f);
                    return { ax + dx * f, ay + dy * f };
                }
                left -= piece;
            }
            return { path[path.size() - 2], path[path.size() - 1] };
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

    void ArmyFigures::update(const MapView& map, const GameData* game, const Movement* movement, MarchClock clock,
                             float time, float dt, const CameraView& camera)
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
        motion_.resize(kArmySlots);

        // Men per army and category, then the live armies by province.
        constexpr std::size_t kCategories = 3;
        men_.assign(std::size_t{ kArmySlots } * kCategories, 0.0f);
        for_each_alive(regiments.alive, [&](std::uint32_t r) {
            const std::uint8_t type = regiments.type[r];
            const auto c = type < category.size() ? static_cast<std::size_t>(category[type]) : 0;
            men_[std::size_t{ regiments.army[r] } * kCategories + c] += static_cast<float>(regiments.strength[r]);
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

        const float idle_yaw = kFacingDegrees * kPi / 180.0f;
        const float px = kBlock / 16.0f;   // world units per model px
        // The walk's speed eases as the game's does (WalkAnimationState: 0.4 of the way a tick).
        const float ease = 1.0f - std::pow(1.0f - kWalkEase, dt * kTicks);
        minecraft::SoldierPose pose{};
        std::array<minecraft::PartRotation, minecraft::horse_part_count> horse_parts{};
        for (std::size_t i = 0; i < order_.size();) {
            const std::uint16_t province = armies.province[order_[i]];
            std::size_t end = i;
            while (end < order_.size() && armies.province[order_[end]] == province) {
                ++end;
            }
            const MapView::Point unit = map.unit_position(province);

            // Where each of the province's armies stands: the first army of each side of its battle (by slot) faces
            // the other's, the side's others are not shown; the rest stand in the row, in slot order.
            place_.assign(end - i, 0);
            bool battle = false;
            bool side_shown[2] = { false, false };
            std::int32_t row = 0;
            for (std::size_t k = i; k < end; ++k) {
                const std::uint32_t army = order_[k];
                if (armies.battle[army] != 0) {
                    const int side = armies.side[army];
                    place_[k - i] = side_shown[side] ? kUnseen : side == 0 ? kAttacker : kDefender;
                    side_shown[side] = true;
                    battle = true;
                } else {
                    place_[k - i] = row++;
                }
            }

            for (std::size_t k = i; k < end; ++k) {
                const std::uint32_t army = order_[k];
                const std::int32_t where = place_[k - i];
                const float target = where == kAttacker ? -kBattleGap * 0.5f * kBlock
                                     : where == kDefender ? kBattleGap * 0.5f * kBlock
                                     : where == kUnseen   ? 0.0f
                                     : battle ? (kBattleGap * 0.5f + kSpacing * static_cast<float>(where + 1)) * kBlock
                                              : (static_cast<float>(where) - static_cast<float>(row - 1) * 0.5f) *
                                                    kSpacing * kBlock;
                Motion& motion = motion_[army];
                if (!motion.seen || motion.generation != armies.generation[army]) {
                    motion = Motion{};
                    motion.yaw = idle_yaw;
                    motion.province = province;
                    motion.generation = armies.generation[army];
                    motion.seen = true;
                }

                // Where on its march: along the step's march path (the curve over the two provinces' land, MapData)
                // as far as the step's progress and, between days, the day's fraction of a day's march take it.
                MapView::Point at = unit;
                float yaw = idle_yaw;
                const std::uint16_t next = armies.next[army];
                const float step =
                    movement != nullptr && next != 0 && next < map.count
                        ? movement->army_step(static_cast<std::uint16_t>(army))
                        : 0.0f;
                const bool marching = step > 0.0f;
                bool arrived = false;   // the figure is at the end of its march before the day that ends it
                std::uint16_t shown_from = province;   // the step the figure is shown on, and how far along
                std::uint16_t shown_to = 0;
                float shown_t = 0.0f;
                if (marching) {
                    // The fraction stands still in the pause, so the figure does too, where it was. Past the end
                    // of the step it walks on into the step after it, as far as the day's carry will take the army
                    // there (so it does not stand at the province's point until the day comes); where the march
                    // ends there, it waits.
                    const float walked = armies.progress[army] + kMarchPerDay * clock.day_fraction;
                    shown_to = next;
                    shown_t = std::clamp(walked / step, 0.0f, 1.0f);
                    const std::uint16_t after = movement->army_after(static_cast<std::uint16_t>(army));
                    const float after_step = after != 0 && after < map.count ? movement->step_cost(next, after) : 0.0f;
                    if (walked > step && after_step > 0.0f) {
                        shown_from = next;
                        shown_to = after;
                        shown_t = std::clamp((walked - step) / after_step, 0.0f, 1.0f);
                    } else if (walked >= step) {
                        arrived = true;
                    }
                    at = along(map, shown_from, shown_to, shown_t, &yaw);
                }
                // In its battle (its march stopped when it joined): facing the other side, east or west.
                const bool fighting = armies.battle[army] != 0 && !marching;
                if (fighting && where != kUnseen) {
                    yaw = where == kAttacker ? kPi * 0.5f : -kPi * 0.5f;
                }

                // A step given up halfway (turned elsewhere or stopped: the same province, another or no next) puts
                // the army back in its province at once; the figure walks back there along the step's path instead
                // of jumping, from where it was shown. An arrival (the province is the old next) and a new march
                // from where it stands go on smoothly.
                if (province == motion.province && motion.next != 0 && next != motion.next &&
                    motion.shown_from == province && motion.shown_to == motion.next) {
                    motion.returning = true;
                    motion.back_to = motion.next;
                    motion.back_t = motion.shown_t;
                }
                motion.province = province;
                motion.next = next;
                if (motion.returning) {
                    const int entry = map.adjacency_entry(province, motion.back_to);
                    const float path = entry >= 0 ? map.march_length[static_cast<std::size_t>(entry)] : 0.0f;
                    motion.back_t = path > 0.0f ? motion.back_t - kReturnSpeed * dt / path : 0.0f;
                    if (motion.back_t > 0.0f) {
                        at = along(map, province, motion.back_to, motion.back_t, &yaw);
                        yaw += kPi;   // walking it backwards
                        shown_to = 0;   // shown on no step of its own: a give-up meanwhile is not walked back
                    } else {
                        motion.returning = false;
                    }
                }
                motion.shown_from = shown_from;
                motion.shown_to = shown_to;
                motion.shown_t = shown_t;
                // It turns toward where it goes; into a battle and out of it, at once (as EU4's units: the user's
                // decision).
                if (fighting != motion.fighting) {
                    motion.yaw = std::remainder(yaw, 2.0f * kPi);
                } else {
                    const float most = kTurnRate * dt;
                    const float turn = std::clamp(std::remainder(yaw - motion.yaw, 2.0f * kPi), -most, most);
                    motion.yaw = std::remainder(motion.yaw + turn, 2.0f * kPi);
                }
                const Vec3 forward{ std::sin(motion.yaw), 0.0f, -std::cos(motion.yaw) };

                // Off its point along x, where its place is (the row, a side of the battle): it is there at once
                // when the province's armies change, as EU4's units are (the user's decision).
                const float x = at.x + target;
                const Vec3 ground{ x, ground_at(map, x, at.y), -at.y };
                const float* m = &men_[std::size_t{ army } * kCategories];
                const bool mounted = m[static_cast<std::size_t>(RegimentCategory::cavalry)] >
                                         m[static_cast<std::size_t>(RegimentCategory::infantry)] &&
                                     m[static_cast<std::size_t>(RegimentCategory::cavalry)] >=
                                         m[static_cast<std::size_t>(RegimentCategory::artillery)];
                // Legs go while it marches and days pass, and while it walks back (in the pause too).
                const bool legs = (marching && !arrived && clock.running) || motion.returning;
                const float walking = legs ? (mounted ? kHorseWalk : kSoldierWalk) : 0.0f;
                motion.amount += (walking - motion.amount) * ease;
                motion.walk = std::fmod(motion.walk + motion.amount * dt * kTicks, kWalkCycle);
                const float age = time * kTicks + static_cast<float>(army) * 13.7f;   // each its own phase

                // The sword: a swing begun runs to its end (in the pause too); while it fights and days pass, a new
                // one every kSwingEvery ticks, give or take; the first a share of that after it joins, so each
                // figure has its own beat.
                if (motion.swing >= 0.0f) {
                    motion.swing += dt * kTicks;
                    if (motion.swing >= minecraft::kSwingTicks) {
                        motion.swing = -1.0f;
                    }
                }
                if (fighting && !motion.fighting) {
                    motion.wait = kSwingEvery * spread(army, motion.swings++);
                }
                motion.fighting = fighting;
                if (fighting && clock.running) {
                    motion.wait -= dt * kTicks;
                    if (motion.wait <= 0.0f && motion.swing < 0.0f) {
                        motion.swing = 0.0f;
                        const float every =
                            kSwingEvery * (1.0f - kSwingSpread + 2.0f * kSwingSpread * spread(army, motion.swings++));
                        motion.wait = std::max(motion.wait + every, 0.0f);
                    }
                }
                const float attack = motion.swing >= 0.0f ? motion.swing / minecraft::kSwingTicks : 0.0f;
                if (where == kUnseen) {
                    continue;
                }

                Placement rider(ground, forward, px);
                if (mounted) {
                    const Placement steed(ground, forward, px * minecraft::kHorseScale);
                    minecraft::horse_pose(age, motion.walk, motion.amount, &horse_parts);
                    draws_.push_back({ horse, static_cast<std::uint32_t>(matrices_.size()) });
                    for (std::size_t p = 0; p < horse_parts.size(); ++p) {
                        matrices_.push_back(steed.part(pivots_[horse][p], horse_parts[p]));
                    }
                    // The rider's ground point on the saddle.
                    const Vec3 seat = steed.point(minecraft::kHorseSeat);
                    rider = Placement(seat, forward, px);
                }
                // A rider does not walk: the horse does.
                minecraft::soldier_pose(age, mounted, mounted ? 0.0f : motion.walk, mounted ? 0.0f : motion.amount,
                                        attack, &pose);
                draws_.push_back({ soldier, static_cast<std::uint32_t>(matrices_.size()) });
                for (std::size_t p = 0; p < pose.turn.size(); ++p) {
                    std::array<float, 3> pivot = pivots_[soldier][p];
                    for (std::size_t c = 0; c < 3; ++c) {
                        pivot[c] += pose.shift[p][c];
                    }
                    matrices_.push_back(rider.part(pivot, pose.turn[p]));
                }

                // The plate under the feet: the ground point on the screen (in front of the camera), lowered by a
                // share of the figure's height there, so the plate leaves the figure's lowest part in view (the
                // feet stand in front of the ground point in the camera's tilt) at every zoom. The figure's box for
                // picking: from the ground point up its height, a share of it to each side.
                const Mat4& vp = camera.view_proj;
                auto screen = [&](Vec3 p, float* sx, float* sy) {
                    const float cx = p.x * vp.m[0][0] + p.y * vp.m[1][0] + p.z * vp.m[2][0] + vp.m[3][0];
                    const float cy = p.x * vp.m[0][1] + p.y * vp.m[1][1] + p.z * vp.m[2][1] + vp.m[3][1];
                    const float cw = p.x * vp.m[0][3] + p.y * vp.m[1][3] + p.z * vp.m[2][3] + vp.m[3][3];
                    if (cw <= 0.0f) {
                        return false;
                    }
                    *sx = (cx / cw * 0.5f + 0.5f) * camera.view_width;
                    *sy = (0.5f - cy / cw * 0.5f) * camera.view_height;
                    return true;
                };
                const float height = (mounted ? kMountedHeight : kStandingHeight) * kBlock;
                float sx = 0.0f;
                float sy = 0.0f;
                float top_x = 0.0f;
                float top_y = 0.0f;
                if (screen(ground, &sx, &sy) && screen(ground + Vec3{ 0.0f, height, 0.0f }, &top_x, &top_y)) {
                    const float tall = std::max(sy - top_y, 0.0f);
                    const float plate_y = sy + tall * kPlateDrop;
                    if (sx > -100.0f && plate_y > -100.0f && sx < camera.view_width + 100.0f &&
                        plate_y < camera.view_height + 100.0f) {
                        markers_.push_back({ static_cast<std::uint16_t>(army), sx, plate_y, top_y, sy,
                                             tall * (mounted ? kMountedHalfWidth : kStandingHalfWidth) });
                    }
                }
            }
            i = end;
        }
    }
}
