#include "minecraft/blocks.h"

#include <algorithm>
#include <cmath>
#include <format>
#include <optional>

#include <nlohmann/json.hpp>

#include "minecraft/assets.h"

namespace mc::minecraft
{
    namespace
    {
        using Json = nlohmann::json;
        using Vec3 = std::array<float, 3>;

        constexpr int kMaxParents = 32;
        constexpr float kPi = 3.14159265358979f;

        std::string_view strip_namespace(std::string_view name)
        {
            const std::size_t colon = name.find(':');
            return colon == std::string_view::npos ? name : name.substr(colon + 1);
        }

        std::optional<Direction> direction(std::string_view name)
        {
            constexpr std::array<std::string_view, 6> names = { "down", "up", "north", "south", "west", "east" };
            for (std::size_t i = 0; i < names.size(); ++i) {
                if (name == names[i] || (name == "bottom" && i == 0)) {
                    return static_cast<Direction>(i);
                }
            }
            return std::nullopt;
        }

        constexpr std::array<Vec3, 6> kNormals = { Vec3{ 0, -1, 0 }, Vec3{ 0, 1, 0 },  Vec3{ 0, 0, -1 },
                                                   Vec3{ 0, 0, 1 },  Vec3{ -1, 0, 0 }, Vec3{ 1, 0, 0 } };

        Direction nearest_direction(const Vec3& n)
        {
            Direction best = Direction::none;
            float most = 0.5f;
            for (std::size_t i = 0; i < kNormals.size(); ++i) {
                const float d = n[0] * kNormals[i][0] + n[1] * kNormals[i][1] + n[2] * kNormals[i][2];
                if (d > most) {
                    most = d;
                    best = static_cast<Direction>(i);
                }
            }
            return best;
        }

        bool read_json(const Assets& assets, const std::string& entry, Json* out)
        {
            std::vector<std::uint8_t> bytes;
            if (!assets.file(entry, &bytes)) {
                return false;
            }
            *out = Json::parse(bytes.begin(), bytes.end(), nullptr, false);
            return out->is_object();
        }

        float number(const Json& value, float fallback = 0.0f)
        {
            return value.is_number() ? value.get<float>() : fallback;
        }

        Vec3 vec3(const Json& value)
        {
            Vec3 v{};
            if (value.is_array() && value.size() == 3) {
                for (std::size_t i = 0; i < 3; ++i) {
                    v[i] = number(value[i]);
                }
            }
            return v;
        }

        // Rotation by `degrees` about one axis through `origin` (right-handed), then, with `rescale`, the other two
        // axes stretched by 1 / cos so a 45-degree plane still spans the block (Minecraft's element rotation).
        Vec3 rotate(const Vec3& p, int axis, float degrees, const Vec3& origin, bool rescale)
        {
            const float a = degrees * kPi / 180.0f;
            const float c = std::cos(a);
            const float s = std::sin(a);
            Vec3 d{ p[0] - origin[0], p[1] - origin[1], p[2] - origin[2] };
            Vec3 r = d;
            if (axis == 0) {
                r[1] = d[1] * c - d[2] * s;
                r[2] = d[1] * s + d[2] * c;
            } else if (axis == 1) {
                r[0] = d[0] * c + d[2] * s;
                r[2] = -d[0] * s + d[2] * c;
            } else {
                r[0] = d[0] * c - d[1] * s;
                r[1] = d[0] * s + d[1] * c;
            }
            if (rescale && std::abs(c) > 1e-4f) {
                for (int i = 0; i < 3; ++i) {
                    if (i != axis) {
                        r[i] /= std::abs(c);
                    }
                }
            }
            return { r[0] + origin[0], r[1] + origin[1], r[2] + origin[2] };
        }

        struct Face
        {
            std::array<float, 4> uv{};
            bool has_uv = false;
            std::string texture;     // "#var" or a texture
            Direction cull = Direction::none;
            int rotation = 0;        // degrees
            bool tinted = false;
        };

        struct Element
        {
            Vec3 from{};
            Vec3 to{};
            int axis = -1;           // -1 = not rotated
            float angle = 0.0f;
            Vec3 origin{ 8, 8, 8 };
            bool rescale = false;
            bool shade = true;
            std::array<std::optional<Face>, 6> faces;
        };

        // The four corners of a face of the box from..to, counter-clockwise seen from outside (Minecraft's FaceInfo).
        std::array<Vec3, 4> corners(Direction d, const Vec3& a, const Vec3& b)
        {
            switch (d) {
            case Direction::down: return { { { a[0], a[1], b[2] }, { a[0], a[1], a[2] }, { b[0], a[1], a[2] }, { b[0], a[1], b[2] } } };
            case Direction::up: return { { { a[0], b[1], a[2] }, { a[0], b[1], b[2] }, { b[0], b[1], b[2] }, { b[0], b[1], a[2] } } };
            case Direction::north: return { { { b[0], b[1], a[2] }, { b[0], a[1], a[2] }, { a[0], a[1], a[2] }, { a[0], b[1], a[2] } } };
            case Direction::south: return { { { a[0], b[1], b[2] }, { a[0], a[1], b[2] }, { b[0], a[1], b[2] }, { b[0], b[1], b[2] } } };
            case Direction::west: return { { { a[0], b[1], a[2] }, { a[0], a[1], a[2] }, { a[0], a[1], b[2] }, { a[0], b[1], b[2] } } };
            default: return { { { b[0], b[1], b[2] }, { b[0], a[1], b[2] }, { b[0], a[1], a[2] }, { b[0], b[1], a[2] } } };
            }
        }

        // The UV rectangle Minecraft gives a face without "uv": the box projected onto the face's plane.
        std::array<float, 4> default_uv(Direction d, const Vec3& a, const Vec3& b)
        {
            switch (d) {
            case Direction::down: return { a[0], 16 - b[2], b[0], 16 - a[2] };
            case Direction::up: return { a[0], a[2], b[0], b[2] };
            case Direction::north: return { 16 - b[0], 16 - b[1], 16 - a[0], 16 - a[1] };
            case Direction::south: return { a[0], 16 - b[1], b[0], 16 - a[1] };
            case Direction::west: return { a[2], 16 - b[1], b[2], 16 - a[1] };
            default: return { 16 - b[2], 16 - b[1], 16 - a[2], 16 - a[1] };
            }
        }

        bool property_matches(const BlockState& state, std::string_view key, std::string_view values)
        {
            const std::string_view have = state.property(key);
            while (true) {
                const std::size_t bar = values.find('|');
                if (values.substr(0, bar) == have) {
                    return true;
                }
                if (bar == std::string_view::npos) {
                    return false;
                }
                values.remove_prefix(bar + 1);
            }
        }

        // A blockstate variant key: "facing=east,half=bottom" (every listed property matches), "" = any.
        bool variant_matches(const BlockState& state, std::string_view key)
        {
            while (!key.empty()) {
                const std::size_t comma = key.find(',');
                const std::string_view pair = key.substr(0, comma);
                const std::size_t eq = pair.find('=');
                if (eq == std::string_view::npos || !property_matches(state, pair.substr(0, eq), pair.substr(eq + 1))) {
                    return false;
                }
                if (comma == std::string_view::npos) {
                    break;
                }
                key.remove_prefix(comma + 1);
            }
            return true;
        }

        // A multipart "when": {"OR": [...]}, {"AND": [...]} or properties, each with values "a|b".
        bool when_matches(const BlockState& state, const Json& when)
        {
            if (!when.is_object()) {
                return true;
            }
            for (const auto& [key, value] : when.items()) {
                if (key == "OR" || key == "AND") {
                    if (!value.is_array()) {
                        return false;
                    }
                    const bool any = key == "OR";
                    bool result = !any;
                    for (const Json& part : value) {
                        const bool m = when_matches(state, part);
                        result = any ? (result || m) : (result && m);
                    }
                    if (!result) {
                        return false;
                    }
                } else {
                    const std::string values = value.is_string() ? value.get<std::string>() : value.dump();
                    if (!property_matches(state, key, values)) {
                        return false;
                    }
                }
            }
            return true;
        }
    }

    std::string_view BlockState::property(std::string_view key) const
    {
        for (const auto& [k, v] : properties) {
            if (k == key) {
                return v;
            }
        }
        return {};
    }

    std::string BlockState::key() const
    {
        std::string out = name;
        if (!properties.empty()) {
            out += '[';
            for (std::size_t i = 0; i < properties.size(); ++i) {
                out += std::format("{}{}={}", i ? "," : "", properties[i].first, properties[i].second);
            }
            out += ']';
        }
        return out;
    }

    void sort_properties(BlockState* state)
    {
        std::ranges::sort(state->properties);
    }

    BlockState parse_block_state(std::string_view text)
    {
        BlockState state;
        const std::size_t open = text.find('[');
        state.name = std::string(strip_namespace(text.substr(0, open)));
        if (open != std::string_view::npos) {
            std::string_view rest = text.substr(open + 1);
            if (!rest.empty() && rest.back() == ']') {
                rest.remove_suffix(1);
            }
            while (!rest.empty()) {
                const std::size_t comma = rest.find(',');
                const std::string_view pair = rest.substr(0, comma);
                const std::size_t eq = pair.find('=');
                if (eq != std::string_view::npos) {
                    state.properties.emplace_back(std::string(pair.substr(0, eq)), std::string(pair.substr(eq + 1)));
                }
                if (comma == std::string_view::npos) {
                    break;
                }
                rest.remove_prefix(comma + 1);
            }
        }
        sort_properties(&state);
        return state;
    }

    BlockTint block_tint(std::string_view block)
    {
        constexpr std::array<std::string_view, 8> grass = { "grass_block", "short_grass", "tall_grass", "fern",
                                                             "large_fern",  "potted_fern", "sugar_cane", "bush" };
        constexpr std::array<std::string_view, 6> foliage = { "oak_leaves",     "jungle_leaves",   "acacia_leaves",
                                                              "dark_oak_leaves", "mangrove_leaves", "vine" };
        if (std::ranges::find(grass, block) != grass.end()) {
            return { BlockTint::Kind::grass };
        }
        if (std::ranges::find(foliage, block) != foliage.end()) {
            return { BlockTint::Kind::foliage };
        }
        if (block == "birch_leaves") {
            return { BlockTint::Kind::fixed, 0x80A755 };   // FoliageColor.FOLIAGE_BIRCH
        }
        if (block == "spruce_leaves") {
            return { BlockTint::Kind::fixed, 0x619961 };   // FoliageColor.FOLIAGE_EVERGREEN
        }
        if (block == "lily_pad") {
            return { BlockTint::Kind::fixed, 0x208030 };
        }
        return {};
    }

    struct BlockModels::Model
    {
        std::unordered_map<std::string, std::string> textures;
        std::vector<Element> elements;
    };

    BlockModels::BlockModels(const Assets& assets) : assets_(assets) {}
    BlockModels::~BlockModels() = default;

    const BlockModels::Model* BlockModels::model(const std::string& name, std::string* error, int depth)
    {
        const std::string key(strip_namespace(name));
        if (const auto found = models_.find(key); found != models_.end()) {
            return found->second.get();
        }
        auto model = std::make_unique<Model>();
        if (!key.starts_with("builtin/")) {
            Json json;
            const std::string entry = "assets/minecraft/models/" + key + ".json";
            if (!read_json(assets_, entry, &json)) {
                if (error) {
                    *error = std::format("{}: no model {}", assets_.path().string(), entry);
                }
                return nullptr;
            }
            if (json.contains("parent") && json["parent"].is_string()) {
                if (depth >= kMaxParents) {
                    if (error) {
                        *error = std::format("model {}: parents nest too deep", key);
                    }
                    return nullptr;
                }
                const Model* parent = this->model(json["parent"].get<std::string>(), error, depth + 1);
                if (!parent) {
                    return nullptr;
                }
                model->textures = parent->textures;
                model->elements = parent->elements;
            }
            if (json.contains("textures") && json["textures"].is_object()) {
                for (const auto& [var, value] : json["textures"].items()) {
                    if (value.is_string()) {
                        model->textures[var] = value.get<std::string>();
                    }
                }
            }
            if (json.contains("elements") && json["elements"].is_array()) {
                model->elements.clear();
                for (const Json& e : json["elements"]) {
                    Element element;
                    element.from = vec3(e.value("from", Json()));
                    element.to = vec3(e.value("to", Json()));
                    element.shade = e.value("shade", true);
                    if (e.contains("rotation") && e["rotation"].is_object()) {
                        const Json& r = e["rotation"];
                        const std::string axis = r.value("axis", std::string("y"));
                        element.axis = axis == "x" ? 0 : axis == "y" ? 1 : 2;
                        element.angle = number(r.value("angle", Json()));
                        element.origin = vec3(r.value("origin", Json::array({ 8, 8, 8 })));
                        element.rescale = r.value("rescale", false);
                    }
                    if (e.contains("faces") && e["faces"].is_object()) {
                        for (const auto& [side, f] : e["faces"].items()) {
                            const std::optional<Direction> d = direction(side);
                            if (!d || !f.is_object()) {
                                continue;
                            }
                            Face face;
                            if (f.contains("uv") && f["uv"].is_array() && f["uv"].size() == 4) {
                                for (std::size_t i = 0; i < 4; ++i) {
                                    face.uv[i] = number(f["uv"][i]);
                                }
                                face.has_uv = true;
                            }
                            face.texture = f.value("texture", std::string());
                            if (f.contains("cullface") && f["cullface"].is_string()) {
                                face.cull = direction(f["cullface"].get<std::string>()).value_or(Direction::none);
                            }
                            face.rotation = static_cast<int>(number(f.value("rotation", Json()), 0.0f));
                            face.tinted = f.contains("tintindex");
                            element.faces[static_cast<std::size_t>(*d)] = face;
                        }
                    }
                    model->elements.push_back(std::move(element));
                }
            }
        }
        const Model* out = model.get();
        models_.emplace(key, std::move(model));
        return out;
    }

    const BakedBlock* BlockModels::bake(const BlockState& state, std::string* error)
    {
        const std::string key = state.key();
        if (const auto found = baked_.find(key); found != baked_.end()) {
            return found->second.get();
        }
        Json blockstate;
        const std::string entry = "assets/minecraft/blockstates/" + state.name + ".json";
        if (!read_json(assets_, entry, &blockstate)) {
            if (error) {
                *error = std::format("{}: no block state {}", assets_.path().string(), entry);
            }
            return nullptr;
        }
        // The models the state shows, each with its x / y rotation.
        std::vector<const Json*> applied;
        auto first = [](const Json& value) -> const Json* {
            if (value.is_array()) {
                return value.empty() ? nullptr : &value[0];   // weighted random models: the first
            }
            return value.is_object() ? &value : nullptr;
        };
        if (blockstate.contains("variants") && blockstate["variants"].is_object()) {
            const Json& variants = blockstate["variants"];
            for (const auto& [variant, value] : variants.items()) {
                if (variant_matches(state, variant)) {
                    applied.push_back(first(value));
                    break;
                }
            }
            if (applied.empty() && !variants.empty()) {
                applied.push_back(first(variants.begin().value()));   // a state the variants do not list
            }
        } else if (blockstate.contains("multipart") && blockstate["multipart"].is_array()) {
            for (const Json& part : blockstate["multipart"]) {
                if (part.is_object() && when_matches(state, part.value("when", Json())) && part.contains("apply")) {
                    applied.push_back(first(part["apply"]));
                }
            }
        }

        auto baked = std::make_unique<BakedBlock>();
        std::size_t elements = 0;
        bool full = false;
        for (const Json* apply : applied) {
            if (!apply || !apply->contains("model") || !(*apply)["model"].is_string()) {
                continue;
            }
            const Model* m = model((*apply)["model"].get<std::string>(), error);
            if (!m) {
                return nullptr;
            }
            const float rx = number(apply->value("x", Json()), 0.0f);
            const float ry = number(apply->value("y", Json()), 0.0f);
            // The variant's rotation: x first, then y, both about the block's centre (Minecraft's rotateYXZ with
            // negated angles: y = 90 turns north to east).
            auto turn = [&](Vec3 p) {
                p = rotate(p, 0, -rx, { 8, 8, 8 }, false);
                return rotate(p, 1, -ry, { 8, 8, 8 }, false);
            };
            auto resolve = [&](std::string texture) {
                for (int i = 0; i < 16 && texture.starts_with('#'); ++i) {
                    const auto found = m->textures.find(texture.substr(1));
                    texture = found == m->textures.end() ? std::string() : found->second;
                }
                return std::string(strip_namespace(texture));
            };
            for (const Element& element : m->elements) {
                ++elements;
                bool all_faces = true;
                for (std::size_t side = 0; side < 6; ++side) {
                    const std::optional<Face>& face = element.faces[side];
                    if (!face) {
                        all_faces = false;
                        continue;
                    }
                    const auto d = static_cast<Direction>(side);
                    ModelQuad quad;
                    quad.texture = resolve(face->texture);
                    if (quad.texture.empty() || quad.texture.starts_with('#')) {
                        continue;
                    }
                    const std::array<Vec3, 4> c = corners(d, element.from, element.to);
                    const std::array<float, 4> uv = face->has_uv ? face->uv : default_uv(d, element.from, element.to);
                    const std::array<std::array<float, 2>, 4> uv_corner = {
                        { { uv[0], uv[1] }, { uv[0], uv[3] }, { uv[2], uv[3] }, { uv[2], uv[1] } }
                    };
                    const int shift = ((face->rotation / 90) % 4 + 4) % 4;
                    for (std::size_t i = 0; i < 4; ++i) {
                        Vec3 p = c[i];
                        if (element.axis >= 0) {
                            p = rotate(p, element.axis, element.angle, element.origin, element.rescale);
                        }
                        p = turn(p);
                        quad.pos[i] = { p[0] / 16.0f, p[1] / 16.0f, p[2] / 16.0f };
                        const auto& t = uv_corner[(i + shift) % 4];
                        quad.uv[i] = { t[0] / 16.0f, t[1] / 16.0f };
                    }
                    if (face->cull != Direction::none) {
                        const Vec3 n = turn({ 8.0f + kNormals[static_cast<std::size_t>(face->cull)][0],
                                              8.0f + kNormals[static_cast<std::size_t>(face->cull)][1],
                                              8.0f + kNormals[static_cast<std::size_t>(face->cull)][2] });
                        quad.cull = nearest_direction({ n[0] - 8.0f, n[1] - 8.0f, n[2] - 8.0f });
                    }
                    quad.tinted = face->tinted;
                    if (element.shade) {
                        const auto& p0 = quad.pos[0];
                        const auto& p1 = quad.pos[1];
                        const auto& p2 = quad.pos[2];
                        const Vec3 a{ p1[0] - p0[0], p1[1] - p0[1], p1[2] - p0[2] };
                        const Vec3 b{ p2[0] - p0[0], p2[1] - p0[1], p2[2] - p0[2] };
                        Vec3 n{ a[1] * b[2] - a[2] * b[1], a[2] * b[0] - a[0] * b[2], a[0] * b[1] - a[1] * b[0] };
                        const float len = std::sqrt(n[0] * n[0] + n[1] * n[1] + n[2] * n[2]);
                        if (len > 1e-6f) {
                            for (float& v : n) {
                                v /= len;
                            }
                            quad.shade = n[0] * n[0] * 0.6f + n[2] * n[2] * 0.8f + n[1] * n[1] * (n[1] > 0 ? 1.0f : 0.5f);
                        }
                    }
                    baked->quads.push_back(std::move(quad));
                }
                if (elements == 1 && all_faces && element.axis < 0 && element.from == Vec3{ 0, 0, 0 } &&
                    element.to == Vec3{ 16, 16, 16 }) {
                    full = true;
                }
            }
        }
        baked->cube = full && elements == 1;
        const BakedBlock* out = baked.get();
        baked_.emplace(key, std::move(baked));
        return out;
    }
}
