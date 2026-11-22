#include "minecraft/voxels.h"

#include <algorithm>
#include <format>
#include <random>
#include <unordered_map>

#include <nlohmann/json.hpp>

#include "minecraft/assets.h"
#include "minecraft/nbt.h"

namespace mc::minecraft
{
    namespace
    {
        using Json = nlohmann::json;

        std::uint64_t pack(int x, int y, int z)
        {
            return (static_cast<std::uint64_t>(x + 32768) << 32) | (static_cast<std::uint64_t>(y + 32768) << 16) |
                   static_cast<std::uint64_t>(z + 32768);
        }

        std::string_view strip_namespace(std::string_view name)
        {
            const std::size_t colon = name.find(':');
            return colon == std::string_view::npos ? name : name.substr(colon + 1);
        }

        bool skipped(std::string_view name)
        {
            return name == "air" || name == "cave_air" || name == "void_air" || name == "structure_void";
        }

        // Random numbers for growing features (java.util.Random's shapes, not its sequence).
        class Random
        {
        public:
            explicit Random(std::uint32_t seed) : engine_(seed) {}
            int next(int bound)   // 0 .. bound - 1
            {
                return bound <= 1 ? 0 : std::uniform_int_distribution<int>(0, bound - 1)(engine_);
            }

        private:
            std::mt19937 engine_;
        };

        // An IntProvider: a number, or {"type": constant | uniform | biased_to_bottom | clamped | weighted_list}.
        int sample(const Json& value, Random& random)
        {
            if (value.is_number_integer()) {
                return value.get<int>();
            }
            if (!value.is_object()) {
                return 0;
            }
            const std::string type(strip_namespace(value.value("type", std::string())));
            const int lo = value.value("min_inclusive", 0);
            const int hi = value.value("max_inclusive", lo);
            if (type == "constant") {
                return value.value("value", 0);
            }
            if (type == "uniform") {
                return lo + random.next(hi - lo + 1);
            }
            if (type == "biased_to_bottom") {
                return lo + random.next(random.next(hi - lo + 1) + 1);
            }
            if (type == "clamped") {
                return std::clamp(sample(value.value("source", Json()), random), value.value("min_inclusive", 0),
                                  value.value("max_inclusive", 0));
            }
            if (type == "weighted_list" && value.contains("distribution") && value["distribution"].is_array()) {
                int total = 0;
                for (const Json& entry : value["distribution"]) {
                    total += entry.value("weight", 1);
                }
                int pick = random.next(std::max(total, 1));
                for (const Json& entry : value["distribution"]) {
                    pick -= entry.value("weight", 1);
                    if (pick < 0) {
                        return sample(entry.value("data", Json()), random);
                    }
                }
            }
            return 0;
        }

        BlockState state_of(const Json& state)
        {
            BlockState out;
            out.name = std::string(strip_namespace(state.value("Name", std::string())));
            if (state.contains("Properties") && state["Properties"].is_object()) {
                for (const auto& [key, value] : state["Properties"].items()) {
                    out.properties.emplace_back(key, value.is_string() ? value.get<std::string>() : value.dump());
                }
            }
            sort_properties(&out);
            return out;
        }

        // A BlockStateProvider: simple, rotated or weighted.
        bool provide(const Json& provider, Random& random, BlockState* out)
        {
            const std::string type(strip_namespace(provider.value("type", std::string())));
            if ((type == "simple_state_provider" || type == "rotated_block_provider") && provider.contains("state")) {
                *out = state_of(provider["state"]);
                return true;
            }
            if (type == "weighted_state_provider" && provider.contains("entries") && provider["entries"].is_array()) {
                int total = 0;
                for (const Json& entry : provider["entries"]) {
                    total += entry.value("weight", 1);
                }
                int pick = random.next(std::max(total, 1));
                for (const Json& entry : provider["entries"]) {
                    pick -= entry.value("weight", 1);
                    if (pick < 0 && entry.contains("data")) {
                        *out = state_of(entry["data"]);
                        return true;
                    }
                }
            }
            return false;
        }

        struct Attachment
        {
            int x = 0;
            int y = 0;
            int z = 0;
            int radius_offset = 0;
        };

        bool grow_tree(const Json& config, Random& random, VoxelModel* out, std::string* error)
        {
            const Json& trunk = config.value("trunk_placer", Json());
            const Json& foliage = config.value("foliage_placer", Json());
            const std::string trunk_type(strip_namespace(trunk.value("type", std::string())));
            const std::string foliage_type(strip_namespace(foliage.value("type", std::string())));
            BlockState log;
            BlockState leaves;
            if (!provide(config.value("trunk_provider", Json()), random, &log) ||
                !provide(config.value("foliage_provider", Json()), random, &leaves)) {
                *error = "tree without a simple trunk or foliage provider";
                return false;
            }
            const int height = trunk.value("base_height", 0) + random.next(trunk.value("height_rand_a", 0) + 1) +
                               random.next(trunk.value("height_rand_b", 0) + 1);

            int foliage_height = 0;
            if (foliage_type == "blob_foliage_placer") {
                foliage_height = foliage.value("height", 3);
            } else if (foliage_type == "spruce_foliage_placer") {
                foliage_height = std::max(4, height - sample(foliage.value("trunk_height", Json()), random));
            } else if (foliage_type != "acacia_foliage_placer") {
                *error = std::format("foliage placer {} is not supported", foliage_type);
                return false;
            }
            const int radius = sample(foliage.value("radius", Json()), random);

            // Trunk.
            std::vector<Attachment> attachments;
            if (trunk_type == "straight_trunk_placer") {
                for (int y = 0; y < height; ++y) {
                    out->set(0, y, 0, log);
                }
                attachments.push_back({ 0, height, 0, 0 });
            } else if (trunk_type == "forking_trunk_placer") {
                // ForkingTrunkPlacer: the trunk leans one way near its top; a second branch may fork off another way.
                constexpr int kStep[4][2] = { { 0, -1 }, { 0, 1 }, { -1, 0 }, { 1, 0 } };
                const int dir = random.next(4);
                const int lean_at = height - random.next(4) - 1;
                int lean = 3 - random.next(3);
                int x = 0;
                int z = 0;
                int top = -1;
                for (int y = 0; y < height; ++y) {
                    if (y >= lean_at && lean > 0) {
                        x += kStep[dir][0];
                        z += kStep[dir][1];
                        --lean;
                    }
                    out->set(x, y, z, log);
                    top = y + 1;
                }
                attachments.push_back({ x, top, z, 1 });
                const int dir2 = random.next(4);
                if (dir2 != dir) {
                    const int from = lean_at - random.next(2) - 1;
                    int length = 1 + random.next(3);
                    x = 0;
                    z = 0;
                    int branch_top = -1;
                    for (int y = from; y < height && length > 0; ++y, --length) {
                        if (y < 1) {
                            continue;
                        }
                        x += kStep[dir2][0];
                        z += kStep[dir2][1];
                        out->set(x, y, z, log);
                        branch_top = y + 1;
                    }
                    if (branch_top >= 0) {
                        attachments.push_back({ x, branch_top, z, 0 });
                    }
                }
            } else {
                *error = std::format("trunk placer {} is not supported", trunk_type);
                return false;
            }

            // Foliage.
            const int offset = sample(foliage.value("offset", Json()), random);
            auto row = [&](const Attachment& a, int range, int y, auto skip) {
                for (int dx = -range; dx <= range; ++dx) {
                    for (int dz = -range; dz <= range; ++dz) {
                        if (!skip(std::abs(dx), y, std::abs(dz), range) && !out->occupied(a.x + dx, a.y + y, a.z + dz)) {
                            out->set(a.x + dx, a.y + y, a.z + dz, leaves);
                        }
                    }
                }
            };
            for (const Attachment& a : attachments) {
                if (foliage_type == "blob_foliage_placer") {
                    auto skip = [&](int dx, int y, int dz, int range) {
                        return dx == range && dz == range && (random.next(2) == 0 || y == 0);
                    };
                    for (int y = offset; y >= offset - foliage_height; --y) {
                        row(a, std::max(radius + a.radius_offset - 1 - y / 2, 0), y, skip);
                    }
                } else if (foliage_type == "spruce_foliage_placer") {
                    auto skip = [](int dx, int, int dz, int range) { return dx == range && dz == range && range > 0; };
                    int r = random.next(2);
                    int widest = 1;
                    int next = 0;
                    for (int y = offset; y >= -foliage_height; --y) {
                        row(a, r, y, skip);
                        if (r >= widest) {
                            r = next;
                            next = 1;
                            widest = std::min(widest + 1, radius + a.radius_offset);
                        } else {
                            ++r;
                        }
                    }
                } else {   // acacia
                    auto skip = [](int dx, int y, int dz, int range) {
                        if (y == 0) {
                            return (dx > 1 || dz > 1) && dx != 0 && dz != 0;
                        }
                        return dx == range && dz == range && range > 0;
                    };
                    const Attachment top{ a.x, a.y + offset, a.z, a.radius_offset };
                    row(top, radius + a.radius_offset, -1 - foliage_height, skip);
                    row(top, radius - 1, -foliage_height, skip);
                    row(top, radius + a.radius_offset - 1, 0, skip);
                }
            }
            return true;
        }
    }

    void VoxelModel::set(int x, int y, int z, const BlockState& state)
    {
        auto found = std::ranges::find(states, state);
        const auto index = static_cast<std::uint32_t>(found - states.begin());
        if (found == states.end()) {
            states.push_back(state);
        }
        for (Voxel& v : voxels) {
            if (v.x == x && v.y == y && v.z == z) {
                v.state = index;
                return;
            }
        }
        voxels.push_back({ x, y, z, index });
    }

    bool VoxelModel::occupied(int x, int y, int z) const
    {
        return std::ranges::any_of(voxels, [&](const Voxel& v) { return v.x == x && v.y == y && v.z == z; });
    }

    bool load_structure(const Assets& assets, std::string_view name, VoxelModel* out, std::string* error)
    {
        const std::string entry = std::format("data/minecraft/structure/{}.nbt", name);
        std::vector<std::uint8_t> bytes;
        Tag root;
        std::string why;
        if (!assets.file(entry, &bytes)) {
            *error = std::format("{}: no {}", assets.path().string(), entry);
            return false;
        }
        if (!read_nbt(bytes, &root, &why)) {
            *error = std::format("{}: {}", entry, why);
            return false;
        }
        const Tag* size = root.get("size");
        const Tag* palette = root.get("palette");
        if (!palette) {
            const Tag* palettes = root.get("palettes");
            palette = palettes && !palettes->items.empty() ? &palettes->items[0] : nullptr;
        }
        const Tag* blocks = root.get("blocks");
        if (!size || size->items.size() != 3 || !palette || !blocks) {
            *error = std::format("{}: not a structure (size, palette, blocks)", entry);
            return false;
        }
        *out = VoxelModel{};
        for (int i = 0; i < 3; ++i) {
            out->size[i] = static_cast<int>(size->items[i].integer);
        }
        std::vector<BlockState> states;
        for (const Tag& p : palette->items) {
            BlockState state;
            const Tag* block = p.get("Name");
            state.name = block ? std::string(strip_namespace(block->text)) : std::string();
            if (const Tag* props = p.get("Properties")) {
                for (const auto& [key, value] : props->fields) {
                    state.properties.emplace_back(key, value.text);
                }
            }
            sort_properties(&state);
            states.push_back(std::move(state));
        }
        for (const Tag& b : blocks->items) {
            const Tag* pos = b.get("pos");
            const Tag* index = b.get("state");
            if (!pos || pos->items.size() != 3 || !index || index->integer < 0 ||
                static_cast<std::size_t>(index->integer) >= states.size()) {
                continue;
            }
            BlockState state = states[static_cast<std::size_t>(index->integer)];
            if (state.name == "jigsaw") {
                const Tag* nbt = b.get("nbt");
                const Tag* final_state = nbt ? nbt->get("final_state") : nullptr;
                state = parse_block_state(final_state ? final_state->text : std::string("air"));
            }
            if (skipped(state.name)) {
                continue;
            }
            out->set(static_cast<int>(pos->items[0].integer), static_cast<int>(pos->items[1].integer),
                     static_cast<int>(pos->items[2].integer), state);
        }
        return true;
    }

    bool grow_feature(const Assets& assets, std::string_view name, std::uint32_t seed, VoxelModel* out,
                      std::string* error)
    {
        const std::string entry = std::format("data/minecraft/worldgen/configured_feature/{}.json", name);
        std::vector<std::uint8_t> bytes;
        if (!assets.file(entry, &bytes)) {
            *error = std::format("{}: no feature {}", assets.path().string(), entry);
            return false;
        }
        const Json json = Json::parse(bytes.begin(), bytes.end(), nullptr, false);
        if (!json.is_object() || !json.contains("config")) {
            *error = std::format("{} is not a configured feature", entry);
            return false;
        }
        const std::string type(strip_namespace(json.value("type", std::string())));
        const Json& config = json["config"];
        Random random(seed);
        *out = VoxelModel{};
        std::string why;
        if (type == "tree") {
            if (!grow_tree(config, random, out, &why)) {
                *error = std::format("{}: {}", entry, why);
                return false;
            }
        } else if (type == "block_column" && config.contains("layers") && config["layers"].is_array()) {
            int y = 0;
            for (const Json& layer : config["layers"]) {
                const int count = sample(layer.value("height", Json()), random);
                BlockState state;
                if (!provide(layer.value("provider", Json()), random, &state)) {
                    continue;
                }
                for (int i = 0; i < count; ++i) {
                    out->set(0, y++, 0, state);
                }
            }
        } else if (type == "simple_block") {
            BlockState state;
            if (!provide(config.value("to_place", Json()), random, &state)) {
                *error = std::format("{}: no simple block to place", entry);
                return false;
            }
            out->set(0, 0, 0, state);
        } else {
            *error = std::format("{}: feature type {} is not supported", entry, type);
            return false;
        }
        std::array<int, 3> lo{ 0, 0, 0 };
        std::array<int, 3> hi{ 0, 0, 0 };
        for (const VoxelModel::Voxel& v : out->voxels) {
            const int p[3] = { v.x, v.y, v.z };
            for (int i = 0; i < 3; ++i) {
                lo[i] = std::min(lo[i], p[i]);
                hi[i] = std::max(hi[i], p[i] + 1);
            }
        }
        for (int i = 0; i < 3; ++i) {
            out->size[i] = hi[i] - lo[i];
        }
        return true;
    }

    bool build_mesh(BlockModels& models, TextureSource& textures, const VoxelModel& model,
                    const std::array<float, 3>& origin, std::vector<MeshVertex>* out, std::string* error)
    {
        std::unordered_map<std::uint64_t, std::uint32_t> at;
        for (std::size_t i = 0; i < model.voxels.size(); ++i) {
            const auto& v = model.voxels[i];
            at[pack(v.x, v.y, v.z)] = model.voxels[i].state;
        }
        // Per state: its baked faces, and whether it is a full block with no transparent texel.
        std::vector<const BakedBlock*> baked(model.states.size(), nullptr);
        std::vector<char> solid(model.states.size(), 0);
        for (std::size_t s = 0; s < model.states.size(); ++s) {
            baked[s] = models.bake(model.states[s], error);
            if (!baked[s]) {
                return false;
            }
            bool opaque = baked[s]->cube;
            for (const ModelQuad& quad : baked[s]->quads) {
                std::uint32_t index = 0;
                if (!textures.index(quad.texture, &index, error)) {
                    return false;
                }
                opaque = opaque && textures.opaque(index);
            }
            solid[s] = opaque ? 1 : 0;
        }
        constexpr int kStep[6][3] = { { 0, -1, 0 }, { 0, 1, 0 }, { 0, 0, -1 }, { 0, 0, 1 }, { -1, 0, 0 }, { 1, 0, 0 } };
        for (const VoxelModel::Voxel& v : model.voxels) {
            const BakedBlock& block = *baked[v.state];
            const BlockTint tint = block_tint(model.states[v.state].name);
            for (const ModelQuad& quad : block.quads) {
                if (quad.cull == Direction::down && v.y == 0) {
                    continue;   // on the ground
                }
                if (quad.cull != Direction::none) {
                    const int* d = kStep[static_cast<std::size_t>(quad.cull)];
                    const auto found = at.find(pack(v.x + d[0], v.y + d[1], v.z + d[2]));
                    if (found != at.end()) {
                        const std::uint32_t other = found->second;
                        const bool same = model.states[other].name == model.states[v.state].name;
                        if (solid[other] || (same && block.cube && baked[other]->cube)) {
                            continue;
                        }
                    }
                }
                std::uint32_t texture = 0;
                if (!textures.index(quad.texture, &texture, error)) {
                    return false;
                }
                MeshVertex corner[4];
                for (int i = 0; i < 4; ++i) {
                    corner[i].x = static_cast<float>(v.x) + quad.pos[i][0] - origin[0];
                    corner[i].y = static_cast<float>(v.y) + quad.pos[i][1] - origin[1];
                    corner[i].z = static_cast<float>(v.z) + quad.pos[i][2] - origin[2];
                    corner[i].u = quad.uv[i][0];
                    corner[i].v = quad.uv[i][1];
                    corner[i].shade = quad.shade;
                    corner[i].texture = texture;
                    corner[i].tint = quad.tinted ? tint : BlockTint{};
                }
                for (const int i : { 0, 1, 2, 0, 2, 3 }) {
                    out->push_back(corner[i]);
                }
            }
        }
        return true;
    }
}
