#include "minecraft/entity_models.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <format>
#include <numbers>

namespace mc::minecraft
{
    namespace
    {
        constexpr float kPi = std::numbers::pi_v<float>;

        struct V3
        {
            float x = 0.0f;
            float y = 0.0f;
            float z = 0.0f;
        };
        V3 operator+(V3 a, V3 b) { return { a.x + b.x, a.y + b.y, a.z + b.z }; }
        V3 operator-(V3 a, V3 b) { return { a.x - b.x, a.y - b.y, a.z - b.z }; }
        V3 operator*(V3 a, float s) { return { a.x * s, a.y * s, a.z * s }; }
        float dot(V3 a, V3 b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
        V3 cross(V3 a, V3 b) { return { a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x }; }

        // A rotation about x (the only box-level turn the models use: the horse's reins).
        V3 turn_x(V3 p, float angle)
        {
            const float c = std::cos(angle);
            const float s = std::sin(angle);
            return { p.x, p.y * c - p.z * s, p.y * s + p.z * c };
        }

        // `top` over `bottom`, both of one size (straight alpha).
        void over(Image* bottom, const Image& top)
        {
            for (std::size_t i = 0; i + 3 < bottom->rgba.size() && i + 3 < top.rgba.size(); i += 4) {
                const float a = top.rgba[i + 3] / 255.0f;
                const float b = bottom->rgba[i + 3] / 255.0f;
                const float out = a + b * (1.0f - a);
                for (int c = 0; c < 3; ++c) {
                    const float v = out > 0.0f ? (top.rgba[i + c] * a + bottom->rgba[i + c] * b * (1.0f - a)) / out : 0.0f;
                    bottom->rgba[i + c] = static_cast<std::uint8_t>(std::lround(v));
                }
                bottom->rgba[i + 3] = static_cast<std::uint8_t>(std::lround(out * 255.0f));
            }
        }

        // Copies a w x h block of `from` at (fx, fy) to (tx, ty) of `to`.
        void copy_block(const Image& from, std::uint32_t fx, std::uint32_t fy, std::uint32_t w, std::uint32_t h,
                        Image* to, std::uint32_t tx, std::uint32_t ty)
        {
            for (std::uint32_t y = 0; y < h; ++y) {
                std::memcpy(&to->rgba[((ty + y) * to->width + tx) * 4], &from.rgba[((fy + y) * from.width + fx) * 4],
                            std::size_t{ w } * 4);
            }
        }

        // Boxes into one part of a model, with the game's box UVs (ModelPart.Cube): the texture's px over the layer.
        class Builder
        {
        public:
            Builder(EntityModel* model, std::uint16_t part, std::uint16_t layer) : model_(model), part_(part), layer_(layer)
            {
            }

            struct Box
            {
                float u = 0.0f;
                float v = 0.0f;
                V3 origin;            // relative to `pivot`
                V3 size;
                float inflate = 0.0f;
                bool mirror = false;
                V3 uv_size{ -1.0f, -1.0f, -1.0f };   // the UV layout's size, if not the box's
                V3 pivot;             // model px; the part's pivot if left at the part's
                bool own_pivot = false;
                float turn_x = 0.0f;  // radians about the pivot
            };

            void box(const Box& b)
            {
                const auto& pp = model_->pivots[part_];
                const V3 part_pivot{ pp[0], pp[1], pp[2] };
                const V3 offset = b.own_pivot ? b.pivot - part_pivot : V3{};
                const float W = b.uv_size.x >= 0.0f ? b.uv_size.x : b.size.x;
                const float H = b.uv_size.y >= 0.0f ? b.uv_size.y : b.size.y;
                const float D = b.uv_size.z >= 0.0f ? b.uv_size.z : b.size.z;
                float x0 = b.origin.x - b.inflate;
                float y0 = b.origin.y - b.inflate;
                float z0 = b.origin.z - b.inflate;
                float x1 = b.origin.x + b.size.x + b.inflate;
                const float y1 = b.origin.y + b.size.y + b.inflate;
                const float z1 = b.origin.z + b.size.z + b.inflate;
                if (b.mirror) {
                    std::swap(x0, x1);   // as the game does: the texture mirrors
                }
                const V3 c[8] = { { x0, y0, z0 }, { x1, y0, z0 }, { x1, y1, z0 }, { x0, y1, z0 },
                                  { x0, y0, z1 }, { x1, y0, z1 }, { x1, y1, z1 }, { x0, y1, z1 } };
                const float u = b.u;
                const float v = b.v;
                const float f4 = u, f5 = u + D, f6 = u + D + W, f7 = u + D + W + W, f8 = u + D + W + D,
                            f9 = u + D + W + D + W;
                const float f10 = v, f11 = v + D, f12 = v + D + H;
                struct Face
                {
                    int corner[4];
                    float uv[4];   // u0, v0, u1, v1
                };
                const Face faces[6] = {
                    { { 5, 4, 0, 1 }, { f5, f10, f6, f11 } },    // top (y0: up)
                    { { 2, 3, 7, 6 }, { f6, f11, f7, f10 } },    // bottom
                    { { 0, 4, 7, 3 }, { f4, f11, f5, f12 } },    // west
                    { { 1, 0, 3, 2 }, { f5, f11, f6, f12 } },    // front
                    { { 5, 1, 2, 6 }, { f6, f11, f8, f12 } },    // east
                    { { 4, 5, 6, 7 }, { f8, f11, f9, f12 } },    // back
                };
                auto place = [&](V3 p) { return offset + turn_x(p, b.turn_x); };
                const V3 centre = place((c[0] + c[6]) * 0.5f);
                for (const Face& f : faces) {
                    V3 p[4];
                    for (int k = 0; k < 4; ++k) {
                        p[k] = place(c[f.corner[k]]);
                    }
                    const float u0 = f.uv[0], v0 = f.uv[1], u1 = f.uv[2], v1 = f.uv[3];
                    const float uvs[4][2] = { { u1, v0 }, { u0, v0 }, { u0, v1 }, { u1, v1 } };
                    quad(p, uvs, centre);
                }
            }

            // A flat quad of 4 points with their UVs (texture px), facing away from `inside`.
            void quad(const V3 (&p)[4], const float (&uv)[4][2], V3 inside)
            {
                V3 n = cross(p[1] - p[0], p[3] - p[0]);
                const float len = std::sqrt(dot(n, n));
                if (len < 1e-6f) {
                    n = cross(p[2] - p[1], p[0] - p[1]);
                    if (std::sqrt(dot(n, n)) < 1e-6f) {
                        return;   // no area (a box of zero thickness): nothing to see edge-on
                    }
                }
                n = n * (1.0f / std::sqrt(dot(n, n)));
                const V3 mid = (p[0] + p[2]) * 0.5f;
                if (dot(n, mid - inside) < 0.0f) {
                    n = n * -1.0f;
                }
                const int order[6] = { 0, 1, 2, 0, 2, 3 };
                for (const int k : order) {
                    EntityVertex vertex{};
                    vertex.pos[0] = p[k].x;
                    vertex.pos[1] = p[k].y;
                    vertex.pos[2] = p[k].z;
                    vertex.normal[0] = n.x;
                    vertex.normal[1] = n.y;
                    vertex.normal[2] = n.z;
                    vertex.uv[0] = uv[k][0] / EntityTextures::kSize;
                    vertex.uv[1] = uv[k][1] / EntityTextures::kSize;
                    vertex.layer = layer_;
                    vertex.part = part_;
                    model_->vertices.push_back(vertex);
                }
            }

        private:
            EntityModel* model_;
            std::uint16_t part_;
            std::uint16_t layer_;
        };

        using Box = Builder::Box;

        // The 4 x 12 x 4 box on a leg: the left piece a hair deeper and longer, so the two pieces' faces never share
        // a plane where they overlap (CinemaShort's leg_piece).
        Box leg_piece(bool left, float u, float v, float inflate, bool mirror)
        {
            const float e = left ? 0.01f : 0.0f;
            Box b;
            b.u = u;
            b.v = v;
            b.origin = { -2.0f, -e, -2.0f - e };
            b.size = { 4.0f, 12.0f + 2.0f * e, 4.0f + 2.0f * e };
            b.uv_size = { 4.0f, 12.0f, 4.0f };
            b.inflate = inflate;
            b.mirror = mirror;
            return b;
        }

        Box box(float u, float v, V3 origin, V3 size, float inflate = 0.0f, bool mirror = false)
        {
            Box b;
            b.u = u;
            b.v = v;
            b.origin = origin;
            b.size = size;
            b.inflate = inflate;
            b.mirror = mirror;
            return b;
        }

        bool load(const Assets& assets, std::string_view name, Image* out, std::string* error)
        {
            if (!assets.image(name, out, error)) {
                return false;
            }
            if (out->width > EntityTextures::kSize || out->height > EntityTextures::kSize) {
                *error = std::format("{}: {} x {}, more than {} px", name, out->width, out->height,
                                     EntityTextures::kSize);
                return false;
            }
            return true;
        }

        // A 16 x 16 block texture (the field gun's).
        bool load_block(const Assets& assets, std::string_view name, Image* out, std::string* error)
        {
            if (!assets.image(name, out, error)) {
                return false;
            }
            if (out->width != 16 || out->height != 16) {
                *error = std::format("{}: {} x {}, not 16 x 16", name, out->width, out->height);
                return false;
            }
            return true;
        }

        // A tile repeated over a whole layer, so a face up to kSize px long reads it on without wrapping.
        Image tiled(const Image& tile)
        {
            Image out;
            out.width = EntityTextures::kSize;
            out.height = EntityTextures::kSize;
            out.rgba.resize(std::size_t{ out.width } * out.height * 4);
            for (std::uint32_t y = 0; y < out.height; ++y) {
                for (std::uint32_t x = 0; x < out.width; ++x) {
                    std::memcpy(&out.rgba[(std::size_t{ y } * out.width + x) * 4],
                                &tile.rgba[(std::size_t{ y % tile.height } * tile.width + x % tile.width) * 4], 4);
                }
            }
            return out;
        }

        // A box as the game's block models have one (an element): each face its own layer, the texture's px laid
        // on it 1:1 from `uv_origin` (part space; the box's `from` if not set) plus the face's offset. Faces: west
        // east (-x +x), up down (-y +y), north south (-z front, +z back). Across a face: z on west and east, else x;
        // down it: z on up and down, else y. `turn` (radians about x, round `pivot`) tips the whole box.
        enum Side : int
        {
            west,
            east,
            up,
            down,
            north,
            south,
        };
        struct Element
        {
            V3 from;
            V3 to;
            std::array<std::uint16_t, 6> layer{};
            std::array<std::array<float, 2>, 6> offset{};
            std::array<bool, 6> hidden{};
            V3 uv_origin;
            bool own_origin = false;
            float turn = 0.0f;
            V3 pivot;
        };
        Element element(V3 from, V3 to, std::uint16_t layer)
        {
            Element e;
            e.from = from;
            e.to = to;
            e.layer.fill(layer);
            return e;
        }
        void add(EntityModel* model, std::uint16_t part, const Element& e)
        {
            const V3 a = e.from;
            const V3 b = e.to;
            const V3 o = e.own_origin ? e.uv_origin : a;
            const V3 c[8] = { { a.x, a.y, a.z }, { b.x, a.y, a.z }, { b.x, b.y, a.z }, { a.x, b.y, a.z },
                              { a.x, a.y, b.z }, { b.x, a.y, b.z }, { b.x, b.y, b.z }, { a.x, b.y, b.z } };
            constexpr int corners[6][4] = { { 0, 4, 7, 3 }, { 1, 5, 6, 2 }, { 0, 1, 5, 4 },
                                            { 3, 2, 6, 7 }, { 0, 1, 2, 3 }, { 4, 5, 6, 7 } };
            auto place = [&](V3 p) { return e.pivot + turn_x(p - e.pivot, e.turn); };
            const V3 centre = place((a + b) * 0.5f);
            for (int s = 0; s < 6; ++s) {
                if (e.hidden[s]) {
                    continue;
                }
                V3 p[4];
                float uv[4][2];
                for (int k = 0; k < 4; ++k) {
                    const V3 q = c[corners[s][k]];
                    p[k] = place(q);
                    uv[k][0] = (s == west || s == east ? q.z - o.z : q.x - o.x) + e.offset[s][0];
                    uv[k][1] = (s == up || s == down ? q.z - o.z : q.y - o.y) + e.offset[s][1];
                }
                Builder(model, part, e.layer[s]).quad(p, uv, centre);
            }
        }

        // `c` (0xAABBGGRR) with its colour times `k`, opaque.
        std::uint32_t shade(std::uint32_t c, float k)
        {
            std::uint32_t out = 0xFF000000u;
            for (int i = 0; i < 3; ++i) {
                const float v = static_cast<float>((c >> (8 * i)) & 0xFFu) * k;
                out |= static_cast<std::uint32_t>(std::lround(std::clamp(v, 0.0f, 255.0f))) << (8 * i);
            }
            return out;
        }
        void put(Image* image, std::uint32_t x, std::uint32_t y, std::uint32_t c)
        {
            std::uint8_t* p = &image->rgba[(std::size_t{ y } * image->width + x) * 4];
            for (int i = 0; i < 4; ++i) {
                p[i] = static_cast<std::uint8_t>((c >> (8 * i)) & 0xFFu);
            }
        }

        // The gun wheel's face, 16 x 16, all wood (the user's design): a rim of the stripped log a little darker
        // past radius 6, 8 spokes of it (0.6 px either side of their lines), the log's end inside radius 2.2 as the
        // hub, the planks dark (0.22) between the spokes.
        Image wheel_face(const Image& planks, const Image& stripped, const Image& end)
        {
            Image out = planks;
            const float eighth = kPi / 4.0f;
            for (std::uint32_t y = 0; y < 16; ++y) {
                for (std::uint32_t x = 0; x < 16; ++x) {
                    const float dx = static_cast<float>(x) + 0.5f - 8.0f;
                    const float dy = static_cast<float>(y) + 0.5f - 8.0f;
                    const float r = std::sqrt(dx * dx + dy * dy);
                    float a = std::fmod(std::atan2(dy, dx), eighth);
                    if (a < 0.0f) {
                        a += eighth;
                    }
                    std::uint32_t c = 0;
                    if (r > 6.0f) {
                        c = shade(stripped.pixel(x, y), 0.78f);
                    } else if (r < 2.2f) {
                        c = end.pixel(x, y);
                    } else if (std::min(a, eighth - a) * r < 0.6f) {
                        c = stripped.pixel(x, y);
                    } else {
                        c = shade(planks.pixel(x, y), 0.22f);
                    }
                    put(&out, x, y, c);
                }
            }
            return out;
        }

        // The item sprite extruded one sprite px thick (the game's generated item models), on a part: sprite px
        // (gx, gy) at `grip` (part space), the sprite's right along `right`, its up along `up`, `scale` model px a
        // sprite px.
        void extrude(const Image& sprite, std::uint16_t layer, EntityModel* model, std::uint16_t part, V3 grip,
                     V3 right, V3 up, float gx, float gy, float scale)
        {
            Builder out(model, part, layer);
            const V3 thick = cross(right, up) * (0.5f * scale);
            auto opaque = [&](int x, int y) {
                return x >= 0 && y >= 0 && x < static_cast<int>(sprite.width) && y < static_cast<int>(sprite.height) &&
                       (sprite.pixel(static_cast<std::uint32_t>(x), static_cast<std::uint32_t>(y)) >> 24) > 25;
            };
            // Sprite point (x right, y down, in sprite px) in part space, on the middle plane.
            auto at = [&](float x, float y) { return grip + right * ((x - gx - 0.5f) * scale) + up * ((gy + 0.5f - y) * scale); };
            const int w = static_cast<int>(sprite.width);
            const int h = static_cast<int>(sprite.height);
            for (int y = 0; y < h; ++y) {
                for (int x = 0; x < w; ++x) {
                    if (!opaque(x, y)) {
                        continue;
                    }
                    const float fx = static_cast<float>(x);
                    const float fy = static_cast<float>(y);
                    const V3 a = at(fx, fy + 1.0f);          // bottom left
                    const V3 b = at(fx + 1.0f, fy + 1.0f);   // bottom right
                    const V3 c = at(fx + 1.0f, fy);          // top right
                    const V3 d = at(fx, fy);                 // top left
                    const V3 mid = at(fx + 0.5f, fy + 0.5f);
                    const float face[4][2] = { { fx, fy + 1.0f }, { fx + 1.0f, fy + 1.0f }, { fx + 1.0f, fy }, { fx, fy } };
                    const float centre[4][2] = { { fx + 0.5f, fy + 0.5f }, { fx + 0.5f, fy + 0.5f },
                                                 { fx + 0.5f, fy + 0.5f }, { fx + 0.5f, fy + 0.5f } };
                    out.quad({ a + thick, b + thick, c + thick, d + thick }, face, mid);
                    out.quad({ a - thick, b - thick, c - thick, d - thick }, face, mid);
                    // Edges where the sprite ends: the pixel's own colour.
                    if (!opaque(x - 1, y)) {
                        out.quad({ a - thick, a + thick, d + thick, d - thick }, centre, mid);
                    }
                    if (!opaque(x + 1, y)) {
                        out.quad({ b - thick, b + thick, c + thick, c - thick }, centre, mid);
                    }
                    if (!opaque(x, y - 1)) {
                        out.quad({ d - thick, d + thick, c + thick, c - thick }, centre, mid);
                    }
                    if (!opaque(x, y + 1)) {
                        out.quad({ a - thick, a + thick, b + thick, b - thick }, centre, mid);
                    }
                }
            }
        }
    }

    std::uint16_t EntityTextures::add(const Image& image)
    {
        const std::size_t layer_bytes = std::size_t{ kSize } * kSize * 4;
        layers.resize(layers.size() + layer_bytes, 0);
        std::uint8_t* to = layers.data() + std::size_t{ count } * layer_bytes;
        for (std::uint32_t y = 0; y < image.height; ++y) {
            std::memcpy(to + std::size_t{ y } * kSize * 4, &image.rgba[std::size_t{ y } * image.width * 4],
                        std::size_t{ image.width } * 4);
        }
        return static_cast<std::uint16_t>(count++);
    }

    bool iron_soldier(const Assets& assets, EntityTextures* textures, EntityModel* out, std::string* error)
    {
        // The skin: base, plains type, weaponsmith; a hand under each sleeve and the palm under the arm, from the
        // base's bare arm (the zombie villager's arms; CinemaShort's make_skins.py).
        Image base;
        Image type;
        Image profession;
        Image iron;
        Image leggings;
        Image sword;
        if (!load(assets, "entity/villager/villager", &base, error) ||
            !load(assets, "entity/villager/type/plains", &type, error) ||
            !load(assets, "entity/villager/profession/weaponsmith", &profession, error) ||
            !load(assets, "entity/equipment/humanoid/iron", &iron, error) ||
            !load(assets, "entity/equipment/humanoid_leggings/iron", &leggings, error) ||
            !load(assets, "item/iron_sword", &sword, error)) {
            return false;
        }
        if (base.width != 64 || base.height != 64 || type.width != 64 || profession.width != 64) {
            *error = "entity/villager: not the 64 x 64 villager textures";
            return false;
        }
        Image skin = base;
        over(&skin, type);
        over(&skin, profession);
        copy_block(base, 44, 30, 16, 4, &skin, 44, 34);
        copy_block(base, 48, 30, 4, 4, &skin, 52, 22);

        const std::uint16_t skin_layer = textures->add(skin);
        const std::uint16_t iron_layer = textures->add(iron);
        const std::uint16_t leggings_layer = textures->add(leggings);
        const std::uint16_t sword_layer = textures->add(sword);

        *out = {};
        out->pivots = { { 0.0f, 0.0f, 0.0f }, { 0.0f, 0.0f, 0.0f }, { -5.0f, 2.0f, 0.0f },
                        { 5.0f, 2.0f, 0.0f }, { -2.0f, 12.0f, 0.0f }, { 2.0f, 12.0f, 0.0f } };
        // Armour inflations a few hundredths off the game's (helmet 1.05, chestplate 1.1, its arms 1.02, leggings
        // 0.6 / 0.65, boots 1.04), so no faces of two pieces share a plane and flicker.
        {
            Builder skin_b(out, villager_head, skin_layer);
            skin_b.box(box(0, 0, { -4, -10, -4 }, { 8, 10, 8 }));
            Box nose = box(24, 0, { -1, -1, -6 }, { 2, 4, 2 });
            nose.pivot = { 0, -2, 0 };
            nose.own_pivot = true;
            skin_b.box(nose);
            Builder(out, villager_head, iron_layer).box(box(0, 0, { -4, -10, -4 }, { 8, 8, 8 }, 1.05f));
        }
        {
            Builder(out, villager_body, skin_layer).box(box(16, 20, { -4, 0, -3 }, { 8, 12, 6 }));
            Box chest = box(16, 16, { -4, 0, -3 }, { 8, 12, 6 }, 1.1f);
            chest.uv_size = { 8, 12, 4 };
            Builder(out, villager_body, iron_layer).box(chest);
            Box belt = box(16, 16, { -4, 0, -3 }, { 8, 12, 6 }, 0.6f);
            belt.uv_size = { 8, 12, 4 };
            Builder(out, villager_body, leggings_layer).box(belt);
        }
        for (const bool left : { false, true }) {
            const std::uint16_t arm = left ? villager_arm_left : villager_arm_right;
            const V3 origin = left ? V3{ -1, -2, -2 } : V3{ -3, -2, -2 };
            Builder(out, arm, skin_layer).box(box(44, 22, origin, { 4, 12, 4 }, 0.0f, left));
            Builder(out, arm, iron_layer).box(box(40, 16, origin, { 4, 12, 4 }, 1.02f, left));
            const std::uint16_t leg = left ? villager_leg_left : villager_leg_right;
            Builder(out, leg, skin_layer).box(box(0, 22, { -2, 0, -2 }, { 4, 12, 4 }, 0.0f, left));
            Builder(out, leg, leggings_layer).box(leg_piece(left, 0, 16, 0.65f, left));
            Builder(out, leg, iron_layer).box(leg_piece(left, 0, 16, 1.04f, left));
        }
        // The sword in the right fist (part space of the right arm, its pivot at the shoulder): the grip px (4, 12)
        // in the hand, the sprite in the arm's side plane, the blade (the sprite's diagonal up and right) straight
        // forward from the fist, away from the body (the user's decision): with the arm raised it points forward
        // and a little up. The game's 0.85.
        constexpr float kHalf = 0.70710678f;
        extrude(sword, sword_layer, out, villager_arm_right, { -1.0f, 9.0f, -0.5f }, { 0, kHalf, -kHalf },
                { 0, -kHalf, -kHalf }, 4.0f, 12.0f, 0.85f);
        return true;
    }

    bool iron_horse(const Assets& assets, EntityTextures* textures, EntityModel* out, std::string* error)
    {
        Image coat;
        Image armour;
        Image saddle;
        if (!load(assets, "entity/horse/horse_brown", &coat, error) ||
            !load(assets, "entity/equipment/horse_body/iron", &armour, error) ||
            !load(assets, "entity/equipment/horse_saddle/saddle", &saddle, error)) {
            return false;
        }
        const std::uint16_t coat_layer = textures->add(coat);
        const std::uint16_t armour_layer = textures->add(armour);
        const std::uint16_t saddle_layer = textures->add(saddle);

        *out = {};
        out->pivots = { { 0.0f, 11.0f, 5.0f },  { 0.0f, 4.0f, -12.0f }, { 4.0f, 14.0f, 7.0f }, { -4.0f, 14.0f, 7.0f },
                        { 4.0f, 14.0f, -10.0f }, { -4.0f, 14.0f, -10.0f }, { 0.0f, 6.0f, 7.0f } };
        // AbstractEquineModel.createBodyMesh; the armour is the same mesh at +0.1 (its body and neck a little more,
        // off the horse's own +0.05 and +0).
        auto equine = [&](std::uint16_t layer, float d, float body, float neck) {
            Builder(out, horse_body, layer).box(box(0, 32, { -5, -8, -17 }, { 10, 10, 22 }, body));
            Builder head(out, horse_neck, layer);
            head.box(box(0, 35, { -2.05f, -6, -2 }, { 4, 12, 7 }, neck));
            head.box(box(0, 13, { -3, -11, -2 }, { 6, 5, 7 }, d));
            head.box(box(56, 36, { -1, -11, 5.01f }, { 2, 16, 2 }, d));
            head.box(box(0, 25, { -2, -11, -7 }, { 4, 5, 5 }, d));
            head.box(box(19, 16, { 0.55f, -13, 4 }, { 2, 3, 1 }, d - 0.001f));
            head.box(box(19, 16, { -2.55f, -13, 4 }, { 2, 3, 1 }, d - 0.001f));
            Builder(out, horse_leg_left_hind, layer).box(box(48, 21, { -3, -1.01f, -1 }, { 4, 11, 4 }, d, true));
            Builder(out, horse_leg_right_hind, layer).box(box(48, 21, { -1, -1.01f, -1 }, { 4, 11, 4 }, d));
            Builder(out, horse_leg_left_front, layer).box(box(48, 21, { -3, -1.01f, -1.9f }, { 4, 11, 4 }, d, true));
            Builder(out, horse_leg_right_front, layer).box(box(48, 21, { -1, -1.01f, -1.9f }, { 4, 11, 4 }, d));
            Builder(out, horse_tail, layer).box(box(42, 36, { -1.5f, 0, 0 }, { 3, 14, 4 }, d));
        };
        equine(coat_layer, 0.0f, 0.05f, 0.0f);
        equine(armour_layer, 0.1f, 0.17f, 0.12f);
        // EquineSaddleModel: the saddle on the body, the harness and reins on the head (the reins off the armoured
        // head's side at 3.1).
        Builder(out, horse_body, saddle_layer).box(box(26, 0, { -5, -8, -9 }, { 10, 9, 9 }, 0.5f));
        Builder harness(out, horse_neck, saddle_layer);
        harness.box(box(29, 5, { 2, -9, -6 }, { 1, 2, 2 }));
        harness.box(box(29, 5, { -3, -9, -6 }, { 1, 2, 2 }));
        for (const float x : { 3.25f, -3.25f }) {
            Box rein = box(32, 2, { x, -6, -8 }, { 0, 3, 16 });
            rein.turn_x = -kPi / 6.0f;
            harness.box(rein);
        }
        harness.box(box(1, 1, { -3, -11, -1.9f }, { 6, 5, 6 }, 0.26f));
        harness.box(box(19, 0, { -2, -11, -4 }, { 4, 5, 2 }, 0.2f));
        return true;
    }

    bool field_gun(const Assets& assets, EntityTextures* textures, EntityModel* out, std::string* error)
    {
        Image planks;
        Image stripped;
        Image end;
        Image iron;
        if (!load_block(assets, "block/spruce_planks", &planks, error) ||
            !load_block(assets, "block/stripped_spruce_log", &stripped, error) ||
            !load_block(assets, "block/spruce_log_top", &end, error) ||
            !load_block(assets, "block/polished_blackstone", &iron, error)) {
            return false;
        }
        // The muzzle's face: the bore, 2 x 2 at the middle of the 6 x 6 face (offset 5 below).
        Image muzzle = iron;
        for (std::uint32_t y = 7; y < 9; ++y) {
            for (std::uint32_t x = 7; x < 9; ++x) {
                put(&muzzle, x, y, 0xFF0E0A0Cu);
            }
        }
        const std::uint16_t wood = textures->add(tiled(planks));
        const std::uint16_t tread = textures->add(tiled(stripped));
        const std::uint16_t wheel = textures->add(tiled(wheel_face(planks, stripped, end)));
        const std::uint16_t metal = textures->add(tiled(iron));
        const std::uint16_t bore = textures->add(tiled(muzzle));

        *out = {};
        out->pivots = { { 0.0f, 16.0f, 0.0f }, { 0.0f, 10.0f, 2.0f }, { 9.0f, 16.0f, 0.0f }, { -9.0f, 16.0f, 0.0f } };
        // The carriage round the axle: the axle (out past the wheels' hubs), two cheeks under the barrel (a hair off
        // its sides), the trail down to the ground behind at 15 degrees.
        add(out, gun_carriage, element({ -11, -1, -1 }, { 11, 1, 1 }, wood));
        add(out, gun_carriage, element({ 3.01f, -4, -4 }, { 5, 2, 10 }, wood));
        add(out, gun_carriage, element({ -5, -4, -4 }, { -3.01f, 2, 10 }, wood));
        Element trail = element({ -2, -1, 6 }, { 2, 3, 30 }, wood);
        trail.turn = -kPi / 12.0f;
        trail.pivot = { 0, -1, 8 };
        add(out, gun_carriage, trail);
        // The barrel round its trunnions: breech, chase, the muzzle's swell with the bore, the cascabel behind.
        add(out, gun_barrel, element({ -3, -3, -4 }, { 3, 3, 7 }, metal));
        Element chase = element({ -2, -2, -16 }, { 2, 2, -4 }, metal);
        chase.hidden[south] = true;
        add(out, gun_barrel, chase);
        Element swell = element({ -3, -3, -19 }, { 3, 3, -16 }, metal);
        swell.layer[north] = bore;
        swell.offset[north] = { 5, 5 };
        add(out, gun_barrel, swell);
        Element cascabel = element({ -1, -1, 7 }, { 1, 1, 9 }, metal);
        cascabel.hidden[north] = true;
        add(out, gun_barrel, cascabel);
        // A wheel: three crossed boxes round its hub, radius 8 (a stepped round), each a hair wider than the one
        // before so their sides never share a plane; the face drawn on the sides, the tread the stripped log.
        for (const std::uint16_t part : { gun_wheel_left, gun_wheel_right }) {
            constexpr float kBoxes[3][3] = { { 1.0f, 7.0f, 7.0f }, { 1.02f, 4.0f, 8.0f }, { 1.04f, 8.0f, 4.0f } };
            for (const auto& half : kBoxes) {
                Element e = element({ -half[0], -half[1], -half[2] }, { half[0], half[1], half[2] }, tread);
                e.layer[west] = wheel;
                e.layer[east] = wheel;
                e.uv_origin = { 0, -8, -8 };
                e.own_origin = true;
                add(out, part, e);
            }
        }
        return true;
    }

    void soldier_pose(float age, bool riding, float walk, float amount, float attack, SoldierPose* out)
    {
        *out = {};
        auto& p = out->turn;
        if (riding) {
            // HumanoidModel while riding: legs forward and apart, arms forward; the sword arm then ArmPose.ITEM
            // (x * 0.5 - pi / 10).
            p[villager_leg_right] = { -1.4137167f, kPi / 10.0f, 0.07853982f };
            p[villager_leg_left] = { -1.4137167f, -kPi / 10.0f, -0.07853982f };
            p[villager_arm_left].x = -kPi / 5.0f;
            p[villager_arm_right].x = -kPi / 5.0f * 0.5f - kPi / 10.0f;
        } else {
            // HumanoidModel.setupAnim's walk: arms cos(walk * 0.6662 [+ pi]) * amount, legs 1.4 times that the
            // other way round; then ArmPose.ITEM on the sword arm (x * 0.5 - pi / 10).
            const float swing = std::cos(walk * 0.6662f);
            const float back = std::cos(walk * 0.6662f + kPi);
            p[villager_arm_right].x = back * amount * 0.5f - kPi / 10.0f;
            p[villager_arm_left].x = swing * amount;
            p[villager_leg_right] = { swing * 1.4f * amount, 0.005f, 0.005f };
            p[villager_leg_left] = { back * 1.4f * amount, -0.005f, -0.005f };
        }
        if (attack > 0.0f) {
            // HumanoidModel.setupAttackAnimation, the right arm (after the arm poses, before the bob): the body turns
            // sin(sqrt(t) * 2pi) * 0.2 and the arms' pivots (at x -5 and 5) go round with it, both arms turn with it
            // and the left one tips by it; the sword arm strikes down by Ease.outQuart, its turn doubled on top
            // (WHACK) and rolled in. The head does not turn with the body; its pitch is 0.
            const float body = std::sin(std::sqrt(attack) * 2.0f * kPi) * 0.2f;
            p[villager_body].y = body;
            const float c = std::cos(body), s = std::sin(body);
            out->shift[villager_arm_right] = { 5.0f - c * 5.0f, 0.0f, s * 5.0f };
            out->shift[villager_arm_left] = { c * 5.0f - 5.0f, 0.0f, -s * 5.0f };
            p[villager_arm_right].y += body;
            p[villager_arm_left].y += body;
            p[villager_arm_left].x += body;
            const float rest = 1.0f - attack;
            const float eased = 1.0f - rest * rest * rest * rest;
            const float strike = std::sin(eased * kPi) * 1.2f + std::sin(attack * kPi) * 0.7f * 0.75f;
            p[villager_arm_right].x -= strike;
            p[villager_arm_right].y += body * 2.0f;
            p[villager_arm_right].z += std::sin(attack * kPi) * -0.4f;
        }
        // AnimationUtils.bobArms: the right arm with +1, the left with -1.
        const float sway = std::cos(age * 0.09f) * 0.05f + 0.05f;
        const float swing = std::sin(age * 0.067f) * 0.05f;
        p[villager_arm_right].z += sway;
        p[villager_arm_right].x += swing;
        p[villager_arm_left].z -= sway;
        p[villager_arm_left].x -= swing;
    }

    void horse_pose(float, float walk, float amount, std::array<PartRotation, horse_part_count>* out)
    {
        auto& p = *out;
        p = {};
        // AbstractEquineModel.setupAnim: the front legs swing cos(walk * 0.6662 + pi) * 0.8 * amount against each
        // other, the hind ones half of it (times the amount again) the other way; the head nods past 0.2 of the
        // speed, the tail lifts with it. At rest the head parts and the tail stand at 30 degrees.
        const float a = std::cos(walk * 0.6662f + kPi) * 0.8f * amount;
        p[horse_leg_left_front].x = a;
        p[horse_leg_right_front].x = -a;
        p[horse_leg_left_hind].x = -a * 0.5f * amount;
        p[horse_leg_right_hind].x = a * 0.5f * amount;
        p[horse_neck].x = kPi / 6.0f + (amount > 0.2f ? std::cos(walk * 0.8f) * 0.15f * amount : 0.0f);
        p[horse_tail].x = kPi / 6.0f + amount * 0.75f;
    }

    void gun_pose(float walk, float shot, GunPose* out)
    {
        *out = {};
        // A shot: the whole gun 4 px back in 2 ticks, then smoothly forward to where it stood by kShotTicks; the
        // barrel's muzzle up by 0.12 rad at tick 1, down again by tick 8.
        constexpr float kRecoil = 4.0f;
        constexpr float kJump = 2.0f;
        float back = 0.0f;
        float kick = 0.0f;
        if (shot >= 0.0f) {
            if (shot < kJump) {
                back = kRecoil * shot / kJump;
            } else {
                const float t = std::min((shot - kJump) / (kShotTicks - kJump), 1.0f);
                back = kRecoil * (1.0f - t * t * (3.0f - 2.0f * t));
            }
            kick = shot < 1.0f ? shot : std::max(0.0f, 1.0f - (shot - 1.0f) / 7.0f);
        }
        for (auto& s : out->shift) {
            s = { 0.0f, 0.0f, back };
        }
        out->turn[gun_barrel].x = -0.12f * kick;
        // The wheels roll with the walk (half the stride's 0.6662 a unit, so a whole number of turns as the walk's
        // position wraps) and back with the jump (radius 8: back / 8).
        const float roll = walk * 0.6662f * 0.5f - back / 8.0f;
        out->turn[gun_wheel_left].x = roll;
        out->turn[gun_wheel_right].x = roll;
    }
}
