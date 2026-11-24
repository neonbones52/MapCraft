#include "app/scenery.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <format>
#include <unordered_map>

#include "view/map_view.h"
#include "minecraft/assets.h"
#include "minecraft/voxels.h"

namespace mc
{
    namespace
    {
        constexpr int kLandBase = 2;             // ground level of height step 0 (tools/mapgen/surface.py LAND_BASE)
        constexpr int kTreeVariants = 6;         // grown copies of every tree feature
        constexpr float kTreeSpan = 0.9f;        // map blocks a tree's crown spans (one tree per block at most)
        constexpr int kNarrowest = 3;            // blocks: narrower plants (cacti, bushes) are scaled as this wide
        constexpr float kTreeJitter = 0.15f;     // map blocks a tree strays from its block's centre
        constexpr float kHouseBlocks = 2.0f;     // map blocks a house site spans
        constexpr int kPierBlocks = 3;           // a pier's blocks per map block (its width)
        constexpr int kPierLength = 2;           // map blocks a pier reaches into the sea (positions.py PIER_BLOCKS)
        constexpr std::string_view kStructures = "data/minecraft/structure/";
        constexpr int kStep[4][2] = { { 0, -1 }, { 1, 0 }, { 0, 1 }, { -1, 0 } };   // north, east, south, west

        std::uint32_t hash(std::uint32_t a, std::uint32_t b, std::uint32_t c)
        {
            std::uint32_t h = a * 374761393u + b * 668265263u + c * 2246822519u + 0x9E3779B9u;
            h = (h ^ (h >> 15)) * 2654435761u;
            h = (h ^ (h >> 13)) * 1274126177u;
            return h ^ (h >> 16);
        }

        float unit(std::uint32_t h)
        {
            return static_cast<float>(h >> 8) / 16777216.0f;
        }

        std::uint32_t name_hash(std::string_view name)
        {
            std::uint32_t h = 2166136261u;
            for (const char c : name) {
                h = (h ^ static_cast<std::uint8_t>(c)) * 16777619u;
            }
            return h;
        }

        std::uint32_t rgba(std::uint32_t rgb)
        {
            return ((rgb >> 16) & 0xFF) | (rgb & 0xFF00) | ((rgb & 0xFF) << 16) | 0xFF000000u;
        }

        // Blocks a village structure has for the ground it stands on: left out, the house stands on the map's blocks.
        bool ground_block(std::string_view name)
        {
            constexpr std::array<std::string_view, 12> ground = { "grass_block", "dirt",     "coarse_dirt", "rooted_dirt",
                                                                  "dirt_path",   "podzol",   "mycelium",    "sand",
                                                                  "red_sand",    "gravel",   "mud",         "farmland" };
            return std::ranges::find(ground, name) != ground.end();
        }

        // Block textures for the models: one layer each (an animation's first frame). Leaves are opaque, as in
        // Minecraft's fast graphics: their holes take the leaf's mean colour, darkened.
        constexpr float kLeafHole = 0.55f;
        class ModelTextures final : public minecraft::TextureSource
        {
        public:
            ModelTextures(const minecraft::Assets& assets, ModelSet* set) : assets_(assets), set_(set) {}

            bool index(const std::string& texture, std::uint32_t* out, std::string* error) override
            {
                if (const auto found = index_.find(texture); found != index_.end()) {
                    *out = found->second;
                    return true;
                }
                minecraft::Texture t;
                if (!assets_.texture(texture, &t, error)) {
                    return false;
                }
                constexpr std::uint32_t size = ModelSet::kTextureSize;
                if (t.frame_size < size || t.order.empty()) {
                    *error = std::format("texture {} is {} px, models need {} px", texture, t.frame_size, size);
                    return false;
                }
                const std::uint32_t y0 = t.order[0] * t.frame_size;
                std::vector<std::uint8_t> texels;
                for (std::uint32_t y = 0; y < size; ++y) {
                    const std::uint8_t* row = &t.image.rgba[(std::size_t{ y0 + y } * t.image.width) * 4];
                    texels.insert(texels.end(), row, row + std::size_t{ size } * 4);
                }
                if (texture.ends_with("_leaves")) {
                    float mean[3] = {};
                    float count = 0.0f;
                    for (std::size_t i = 0; i < texels.size(); i += 4) {
                        if (texels[i + 3] >= 128) {
                            for (int c = 0; c < 3; ++c) {
                                mean[c] += texels[i + c];
                            }
                            count += 1.0f;
                        }
                    }
                    for (std::size_t i = 0; i < texels.size(); i += 4) {
                        if (texels[i + 3] < 128) {
                            for (int c = 0; c < 3; ++c) {
                                texels[i + c] = static_cast<std::uint8_t>(mean[c] / std::max(count, 1.0f) * kLeafHole);
                            }
                        }
                        texels[i + 3] = 255;
                    }
                }
                bool opaque = true;
                for (std::size_t i = 3; i < texels.size(); i += 4) {
                    opaque = opaque && texels[i] == 255;
                }
                set_->layers.insert(set_->layers.end(), texels.begin(), texels.end());
                *out = set_->layer_count++;
                opaque_.push_back(opaque ? 1 : 0);
                index_.emplace(texture, *out);
                return true;
            }

            [[nodiscard]] bool opaque(std::uint32_t index) const override
            {
                return index < opaque_.size() && opaque_[index] != 0;
            }

        private:
            const minecraft::Assets& assets_;
            ModelSet* set_;
            std::unordered_map<std::string, std::uint32_t> index_;
            std::vector<char> opaque_;
        };

        // Meshes of the set, built from voxel models with the jar's block models and textures.
        class Meshes
        {
        public:
            Meshes(const minecraft::Assets& assets, ModelSet* set) : models_(assets), textures_(assets, set), set_(set)
            {
            }

            bool add(const std::string& key, ModelSet::Group group, const minecraft::VoxelModel& model,
                     const std::array<float, 3>& origin, std::uint32_t* out, std::string* error)
            {
                std::vector<minecraft::MeshVertex> vertices;
                if (!minecraft::build_mesh(models_, textures_, model, origin, &vertices, error)) {
                    *error = std::format("{}: {}", key, *error);
                    return false;
                }
                ModelSet::Mesh mesh;
                mesh.first = static_cast<std::uint32_t>(set_->vertices.size());
                mesh.count = static_cast<std::uint32_t>(vertices.size());
                mesh.group = group;
                for (const minecraft::MeshVertex& v : vertices) {
                    ModelSet::Vertex out_vertex{};
                    out_vertex.pos[0] = v.x;
                    out_vertex.pos[1] = v.y;
                    out_vertex.pos[2] = v.z;
                    out_vertex.uv[0] = v.u;
                    out_vertex.uv[1] = v.v;
                    const auto shade = static_cast<std::uint32_t>(std::clamp(v.shade, 0.0f, 1.0f) * 255.0f + 0.5f);
                    out_vertex.layer_shade_tint =
                        (v.texture & 0xFFFF) | (shade << 16) | (static_cast<std::uint32_t>(v.tint.kind) << 24);
                    out_vertex.tint = rgba(v.tint.rgb);
                    set_->vertices.push_back(out_vertex);
                }
                *out = static_cast<std::uint32_t>(set_->meshes.size());
                set_->meshes.push_back(mesh);
                return true;
            }

        private:
            minecraft::BlockModels models_;
            ModelTextures textures_;
            ModelSet* set_;
        };

        // A pier pointing north (-z) from the coast edge at the origin, in blocks of a third of a map block: one map
        // block wide, kPierLength map blocks long. A row of planks down its middle, its top level with the land of
        // step 0; on both sides every other block a pile (a dark fence) from the water up to the planks, joined to
        // them by its rail as Minecraft joins a fence to a block beside it.
        minecraft::VoxelModel pier()
        {
            using minecraft::parse_block_state;
            minecraft::VoxelModel model;
            const auto deck = parse_block_state("oak_planks");
            const auto post = parse_block_state("dark_oak_fence");
            const auto joined_east = parse_block_state("dark_oak_fence[east=true]");
            const auto joined_west = parse_block_state("dark_oak_fence[west=true]");
            const int length = kPierLength * kPierBlocks;
            const int level = kPierBlocks - 1;   // the deck's layer
            for (int z = -length; z < 0; ++z) {
                model.set(1, level, z, deck);
            }
            for (int z = -2; z >= -length; z -= 2) {
                for (int y = 0; y <= level; ++y) {
                    model.set(0, y, z, y == level ? joined_east : post);
                    model.set(2, y, z, y == level ? joined_west : post);
                }
            }
            model.size = { kPierBlocks, level + 1, length };
            return model;
        }
    }

    bool build_scenery(const MapView& map, const std::vector<const BiomeRow*>& biomes, const TerrainLook& look,
                       const minecraft::Assets& assets, ModelSet* out, std::string* error)
    {
        *out = ModelSet{};
        Meshes meshes(assets, out);
        const auto w = static_cast<int>(map.block_width);
        const auto h = static_cast<int>(map.block_height);
        const float bs = static_cast<float>(map.block_size);   // world units per map block
        auto index = [&](int x, int y) { return static_cast<std::size_t>(y) * w + x; };
        auto row_at = [&](std::size_t i) -> const BiomeRow* {
            const std::uint8_t b = map.block_biome[i];
            return b < biomes.size() ? biomes[b] : nullptr;
        };

        // Foliage colour per map biome: the vanilla biome's, else its colour map.
        minecraft::Image foliage_map;
        if (!assets.image("colormap/foliage", &foliage_map, error)) {
            return false;
        }
        std::vector<std::uint32_t> foliage(biomes.size(), rgba(0x48B518));
        for (std::size_t b = 1; b < biomes.size(); ++b) {
            minecraft::Biome vanilla;
            if (biomes[b] && assets.biome(biomes[b]->vanilla, &vanilla, error)) {
                foliage[b] = rgba(vanilla.foliage_color.value_or(
                    minecraft::colormap_color(foliage_map, vanilla.temperature, vanilla.downfall)));
            }
        }
        auto place = [&](std::uint32_t mesh, float x, float y, float z, float scale, std::uint32_t turn,
                         std::size_t block, std::uint32_t flags = 0) {
            ModelSet::Instance instance{};
            instance.pos[0] = x;
            instance.pos[1] = y;
            instance.pos[2] = z;
            instance.scale = scale;
            instance.mesh = mesh;
            instance.turn_flags = (turn & ModelSet::kTurnMask) | flags;
            instance.foliage = foliage[std::min<std::size_t>(map.block_biome[block], foliage.size() - 1)];
            instance.grass = block < look.grass_tint.size() ? look.grass_tint[block] : 0xFFFFFFFFu;
            out->instances.push_back(instance);
        };
        std::vector<std::uint8_t> blocked(static_cast<std::size_t>(w) * h, 0);   // no trees here

        // Houses.
        std::unordered_map<std::string, std::vector<std::string>> houses;   // per village style
        std::unordered_map<std::string, std::pair<std::uint32_t, float>> house_meshes;   // mesh, footprint (blocks)
        for (std::uint32_t pid = 1; pid < map.count; ++pid) {
            const std::uint16_t* site = &map.sites[std::size_t{ pid } * 6];
            if (map.kind[pid] != ProvinceKind::land || site[0] == MapView::kNoSite) {
                continue;
            }
            const int bx = site[0];
            const int by = site[1];
            if (bx + 1 >= w || by + 1 >= h) {
                continue;
            }
            int ground = 0;
            for (int k = 0; k < 4; ++k) {
                const std::size_t i = index(bx + k % 2, by + k / 2);
                ground = std::max<int>(ground, map.block_ground[i]);
                blocked[i] = 1;
            }
            const std::size_t at = index(bx, by);
            const BiomeRow* row = row_at(at);
            if (!row) {
                continue;
            }
            std::string style = row->village;
            if (row->snow_line >= 0 && ground - kLandBase >= row->snow_line) {
                style = "snowy";
            }
            auto& list = houses[style];
            if (list.empty()) {
                for (std::string entry :
                     assets.list(std::format("{}village/{}/houses/{}_small_house_", kStructures, style, style))) {
                    if (entry.ends_with(".nbt")) {
                        list.push_back(entry.substr(kStructures.size(), entry.size() - kStructures.size() - 4));
                    }
                }
                if (list.empty()) {
                    *error = std::format("{}: no {} village small houses", assets.path().string(), style);
                    return false;
                }
            }
            const std::string& name = list[hash(pid, 1, 0) % list.size()];
            auto made = house_meshes.find(name);
            if (made == house_meshes.end()) {
                minecraft::VoxelModel model;
                std::uint32_t mesh = 0;
                if (!minecraft::load_structure(assets, name, &model, error)) {
                    return false;
                }
                // Without the ground it was built into, standing on the site from its lowest block left, the
                // footprint centred.
                std::erase_if(model.voxels, [&](const minecraft::VoxelModel::Voxel& v) {
                    return ground_block(model.states[v.state].name);
                });
                int lowest = model.size[1];
                for (const auto& v : model.voxels) {
                    lowest = std::min(lowest, v.y);
                }
                for (auto& v : model.voxels) {
                    v.y -= lowest;
                }
                if (!meshes.add("house " + name, ModelSet::Group::settlements, model,
                                { model.size[0] * 0.5f, 0.0f, model.size[2] * 0.5f }, &mesh, error)) {
                    return false;
                }
                const auto footprint = static_cast<float>(std::max({ model.size[0], model.size[2], 1 }));
                made = house_meshes.emplace(name, std::pair{ mesh, footprint }).first;
            }
            // A Minecraft block is a map pixel, shrunk where a house would spill off its site.
            const auto [mesh, footprint] = made->second;
            const float scale = std::min(bs / 4.0f, kHouseBlocks * bs / footprint);
            place(mesh, (bx + 1) * bs, (ground - 1) * bs, -(by + 1) * bs, scale, site[2], at);
        }

        // Piers.
        std::uint32_t pier_mesh = 0;
        if (!meshes.add("pier", ModelSet::Group::settlements, pier(), { kPierBlocks * 0.5f, 0.0f, 0.0f },
                        &pier_mesh, error)) {
            return false;
        }
        for (std::uint32_t pid = 1; pid < map.count; ++pid) {
            const std::uint16_t* site = &map.sites[std::size_t{ pid } * 6];
            if (site[3] == MapView::kNoSite || site[5] > 3) {
                continue;
            }
            const int bx = site[3];
            const int by = site[4];
            if (bx >= w || by >= h) {
                continue;
            }
            blocked[index(bx, by)] = 1;
            const int* d = kStep[site[5]];
            const float x = (bx + 0.5f + d[0] * 0.5f) * bs;
            const float y = (by + 0.5f + d[1] * 0.5f) * bs;
            place(pier_mesh, x, 0.0f, -y, bs / static_cast<float>(kPierBlocks), site[5], index(bx, by));
        }

        // Trees.
        std::unordered_map<std::string, std::uint32_t> first_variant;   // feature -> its first mesh
        std::unordered_map<std::string, float> feature_scale;           // world units per block, by its widest variant
        for (std::size_t b = 1; b < biomes.size(); ++b) {
            if (!biomes[b]) {
                continue;
            }
            for (const std::string& feature : biomes[b]->trees) {
                if (first_variant.contains(feature)) {
                    continue;
                }
                int widest = kNarrowest;
                for (int k = 0; k < kTreeVariants; ++k) {
                    minecraft::VoxelModel model;
                    if (!minecraft::grow_feature(assets, feature, name_hash(feature) + static_cast<std::uint32_t>(k),
                                                 &model, error)) {
                        *error = std::format("biomes.csv: {} trees: {}", biomes[b]->key, *error);
                        return false;
                    }
                    std::uint32_t mesh = 0;
                    if (!meshes.add(std::format("tree {} {}", feature, k), ModelSet::Group::trees, model,
                                    { 0.5f, 0.0f, 0.5f }, &mesh, error)) {
                        return false;
                    }
                    if (k == 0) {
                        first_variant[feature] = mesh;
                    }
                    widest = std::max({ widest, model.size[0], model.size[2] });
                }
                feature_scale[feature] = kTreeSpan * bs / static_cast<float>(widest);
            }
        }
        for (int y = 0; y < h; ++y) {
            for (int x = 0; x < w; ++x) {
                const std::size_t i = index(x, y);
                const BlockClass cls = map.block_class[i];
                if ((cls != BlockClass::land && cls != BlockClass::wasteland) || blocked[i]) {
                    continue;
                }
                const BiomeRow* row = row_at(i);
                if (!row || row->trees.empty() || row->tree_density <= 0.0f) {
                    continue;
                }
                const int step = map.block_ground[i] - kLandBase;
                if (row->tree_line >= 0 && step >= row->tree_line) {
                    continue;
                }
                bool shore = false;
                for (int dy = -1; dy <= 1 && !shore; ++dy) {
                    for (int dx = -1; dx <= 1; ++dx) {
                        const int xx = x + dx;
                        const int yy = y + dy;
                        if (xx >= 0 && yy >= 0 && xx < w && yy < h) {
                            const BlockClass other = map.block_class[index(xx, yy)];
                            shore = shore || other == BlockClass::sea || other == BlockClass::lake ||
                                    other == BlockClass::river;
                        }
                    }
                }
                if (shore) {
                    continue;
                }
                const auto ux = static_cast<std::uint32_t>(x);
                const auto uy = static_cast<std::uint32_t>(y);
                if (unit(hash(ux, uy, 0)) >= row->tree_density) {
                    continue;
                }
                const std::string& feature = row->trees[hash(ux, uy, 1) % row->trees.size()];
                const std::uint32_t mesh = first_variant[feature] + hash(ux, uy, 2) % kTreeVariants;
                const float jx = (unit(hash(ux, uy, 3)) * 2.0f - 1.0f) * kTreeJitter;
                const float jy = (unit(hash(ux, uy, 4)) * 2.0f - 1.0f) * kTreeJitter;
                const float cx = (static_cast<float>(x) + 0.5f + jx) * bs;
                const float cy = (static_cast<float>(y) + 0.5f + jy) * bs;
                place(mesh, cx, static_cast<float>(map.block_ground[i] - 1) * bs, -cy, feature_scale[feature],
                      hash(ux, uy, 5), i, ModelSet::kFilled);   // trees take the map mode's colour, as the blocks
            }
        }
        return true;
    }
}
