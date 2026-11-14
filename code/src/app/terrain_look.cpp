#include "app/terrain_look.h"

#include <algorithm>
#include <array>
#include <charconv>
#include <cmath>
#include <format>
#include <unordered_map>

#include "core/csv.h"
#include "core/map_data.h"
#include "minecraft/assets.h"

namespace mc
{
    namespace
    {
        constexpr std::uint32_t kVariants = 3;   // per biome: its own blocks, shore, snow
        constexpr std::uint32_t kOwn = 0;
        constexpr std::uint32_t kShore = 1;
        constexpr std::uint32_t kSnow = 2;
        constexpr int kBlend = 2;                // blocks each way: 5 x 5, Minecraft's default biome blend
        constexpr int kLandBase = 2;             // ground level of height step 0 (tools/mapgen/surface.py LAND_BASE)
        constexpr std::uint32_t kDefaultWater = 0x3F76E4;

        struct BiomeRow
        {
            std::string key;
            std::string vanilla;
            std::array<std::string, 7> blocks;   // top, side, under, deep, shore, snow_top, snow_side
            int snow_line = -1;                  // height step, -1 = never
        };

        std::uint32_t rgba(std::uint32_t rgb)
        {
            return ((rgb >> 16) & 0xFF) | (rgb & 0xFF00) | ((rgb & 0xFF) << 16) | 0xFF000000u;
        }

        // Value noise in -1..1, smooth between lattice points: Minecraft's swamp grass picks one of two colours by
        // its biome noise; any smooth noise of the same scale reads alike.
        float lattice(int x, int y)
        {
            std::uint32_t h = static_cast<std::uint32_t>(x) * 374761393u + static_cast<std::uint32_t>(y) * 668265263u;
            h = (h ^ (h >> 13)) * 1274126177u;
            return static_cast<float>(h ^ (h >> 16)) / 2147483648.0f - 1.0f;
        }

        float noise(float x, float y)
        {
            const float fx = std::floor(x);
            const float fy = std::floor(y);
            const int ix = static_cast<int>(fx);
            const int iy = static_cast<int>(fy);
            auto fade = [](float t) { return t * t * (3.0f - 2.0f * t); };
            const float u = fade(x - fx);
            const float v = fade(y - fy);
            const float a = lattice(ix, iy) + (lattice(ix + 1, iy) - lattice(ix, iy)) * u;
            const float b = lattice(ix, iy + 1) + (lattice(ix + 1, iy + 1) - lattice(ix, iy + 1)) * u;
            return a + (b - a) * v;
        }

        // Grass of a vanilla biome at block (x, y): its colour map colour or override, with its modifier.
        std::uint32_t grass_color(const minecraft::Biome& biome, std::uint32_t base, int x, int y)
        {
            if (biome.grass_modifier == "swamp") {
                return noise(static_cast<float>(x) * 0.0225f, static_cast<float>(y) * 0.0225f) < -0.1f ? 0x4C763C
                                                                                                         : 0x6A7039;
            }
            if (biome.grass_modifier == "dark_forest") {
                return ((base & 0xFEFEFE) + 0x28340A) >> 1;
            }
            return base;
        }

        // Every block's colour averaged over the blocks round it that have one (weight > 0); blocks with none round
        // them keep their own.
        std::vector<std::uint32_t> blend(const std::vector<std::uint32_t>& rgb, const std::vector<std::uint8_t>& weight,
                                         int w, int h)
        {
            std::vector<std::array<float, 4>> rows(rgb.size());   // r, g, b, weight summed along x
            for (int y = 0; y < h; ++y) {
                for (int x = 0; x < w; ++x) {
                    std::array<float, 4> sum{};
                    for (int dx = -kBlend; dx <= kBlend; ++dx) {
                        const int xx = std::clamp(x + dx, 0, w - 1);
                        const std::size_t i = static_cast<std::size_t>(y) * w + xx;
                        if (weight[i]) {
                            sum[0] += static_cast<float>((rgb[i] >> 16) & 0xFF);
                            sum[1] += static_cast<float>((rgb[i] >> 8) & 0xFF);
                            sum[2] += static_cast<float>(rgb[i] & 0xFF);
                            sum[3] += 1.0f;
                        }
                    }
                    rows[static_cast<std::size_t>(y) * w + x] = sum;
                }
            }
            std::vector<std::uint32_t> out(rgb.size());
            for (int y = 0; y < h; ++y) {
                for (int x = 0; x < w; ++x) {
                    std::array<float, 4> sum{};
                    for (int dy = -kBlend; dy <= kBlend; ++dy) {
                        const auto& r = rows[static_cast<std::size_t>(std::clamp(y + dy, 0, h - 1)) * w + x];
                        for (int c = 0; c < 4; ++c) {
                            sum[c] += r[c];
                        }
                    }
                    const std::size_t i = static_cast<std::size_t>(y) * w + x;
                    if (sum[3] <= 0.0f) {
                        out[i] = rgba(rgb[i]);
                        continue;
                    }
                    auto channel = [&](int c) { return static_cast<std::uint32_t>(sum[c] / sum[3] + 0.5f); };
                    out[i] = channel(0) | (channel(1) << 8) | (channel(2) << 16) | 0xFF000000u;
                }
            }
            return out;
        }

        class TextureSet
        {
        public:
            TextureSet(const minecraft::Assets& assets, TerrainLook* look) : assets_(assets), look_(look) {}

            // The index of block texture `name` (assets/minecraft/textures/block/<name>.png), loaded on first use.
            bool get(const std::string& name, std::uint32_t* out, std::string* error)
            {
                if (const auto found = index_.find(name); found != index_.end()) {
                    *out = found->second;
                    return true;
                }
                minecraft::Texture texture;
                if (!assets_.texture("block/" + name, &texture, error)) {
                    return false;
                }
                constexpr std::uint32_t size = TerrainLook::kTextureSize;
                if (texture.frame_size < size) {
                    if (error) {
                        *error = std::format("texture block/{} is {} px, the map needs {} px", name, texture.frame_size,
                                             size);
                    }
                    return false;
                }
                TerrainLook::Texture info;
                info.first_layer = look_->layer_count;
                info.frames = static_cast<std::uint32_t>(texture.order.size());
                info.frame_time = texture.frame_time;
                info.tint = name == "grass_block_top" || name.ends_with("_overlay") ? TerrainLook::Tint::grass
                            : name.starts_with("water_")                            ? TerrainLook::Tint::water
                                                                                    : TerrainLook::Tint::none;
                // Each frame's top-left 16 x 16 (a larger frame, as flowing water's 32 x 32, is shown at block scale
                // the way Minecraft shows it).
                for (const std::uint32_t frame : texture.order) {
                    const std::uint32_t y0 = frame * texture.frame_size;
                    for (std::uint32_t y = 0; y < size; ++y) {
                        const std::uint8_t* row =
                            &texture.image.rgba[(std::size_t{ y0 + y } * texture.image.width) * 4];
                        look_->layers.insert(look_->layers.end(), row, row + std::size_t{ size } * 4);
                    }
                    ++look_->layer_count;
                }
                *out = static_cast<std::uint32_t>(look_->textures.size());
                look_->textures.push_back(info);
                index_.emplace(name, *out);
                return true;
            }

        private:
            const minecraft::Assets& assets_;
            TerrainLook* look_;
            std::unordered_map<std::string, std::uint32_t> index_;
        };

        bool read_biomes(const std::filesystem::path& path, std::vector<BiomeRow>* rows, std::string* error)
        {
            csv::Table table;
            if (!csv::read(path, &table, error)) {
                return false;
            }
            const std::array<const char*, 10> names = { "key",  "minecraft_biome", "top",      "side",      "under",
                                                        "deep", "shore",           "snow_top", "snow_side", "snow_line" };
            std::array<int, 10> col{};
            for (std::size_t c = 0; c < names.size(); ++c) {
                col[c] = table.column(names[c]);
                if (col[c] < 0) {
                    *error = std::format("{}: no column {}", path.string(), names[c]);
                    return false;
                }
            }
            for (std::size_t r = 0; r < table.rows.size(); ++r) {
                const auto& row = table.rows[r];
                BiomeRow biome;
                biome.key = row[col[0]];
                biome.vanilla = row[col[1]];
                const std::string& line = row[col[9]];
                if (!line.empty()) {
                    const auto [end, ec] = std::from_chars(line.data(), line.data() + line.size(), biome.snow_line);
                    if (ec != std::errc() || end != line.data() + line.size() || biome.snow_line < 0) {
                        *error = std::format("{}:{}: snow_line '{}' is not a height step", path.string(),
                                             table.lines[r], line);
                        return false;
                    }
                }
                // Snow blocks are needed only with a snow line; without one they stay the biome's own.
                for (std::size_t b = 0; b < biome.blocks.size(); ++b) {
                    biome.blocks[b] = row[col[2 + b]];
                    const bool snow = b >= 5;
                    if (biome.blocks[b].empty() && snow && biome.snow_line < 0) {
                        biome.blocks[b] = biome.blocks[b - 5];
                    }
                    if (biome.blocks[b].empty()) {
                        *error = std::format("{}:{}: {} has no {}", path.string(), table.lines[r], biome.key,
                                             names[2 + b]);
                        return false;
                    }
                }
                if (biome.vanilla.empty()) {
                    *error = std::format("{}:{}: {} has no minecraft_biome", path.string(), table.lines[r], biome.key);
                    return false;
                }
                rows->push_back(std::move(biome));
            }
            return true;
        }
    }

    bool build_terrain_look(const MapData& map, const std::filesystem::path& biomes_csv,
                            const minecraft::Assets& assets, TerrainLook* look, std::string* error)
    {
        std::vector<BiomeRow> table;
        if (!read_biomes(biomes_csv, &table, error)) {
            return false;
        }
        // The map's biomes (BKEY order, [0] = none) as biomes.csv rows.
        const std::uint32_t biome_count = map.biome_count();
        if (biome_count < 2 || (biome_count - 1) * kVariants > 256) {
            *error = std::format("the map has {} biomes; the renderer takes 1 to {}", biome_count - 1, 256 / kVariants);
            return false;
        }
        std::vector<const BiomeRow*> biomes(biome_count, nullptr);
        for (std::uint32_t b = 1; b < biome_count; ++b) {
            const std::string_view key = map.biome_key(b);
            const auto row = std::ranges::find(table, key, &BiomeRow::key);
            if (row == table.end()) {
                *error = std::format("{}: no biome {} (the map was baked with it; rebake or add it)",
                                     biomes_csv.string(), key);
                return false;
            }
            biomes[b] = &*row;
        }

        *look = TerrainLook{};
        TextureSet textures(assets, look);
        auto texture = [&](const std::string& name, std::uint32_t* out) {
            if (!textures.get(name, out, error)) {
                *error = std::format("{}: {}", biomes_csv.string(), *error);
                return false;
            }
            return true;
        };
        if (!texture("water_still", &look->water_still) || !texture("water_flow", &look->water_flow)) {
            return false;
        }
        minecraft::Image grass_map;
        if (!assets.image("colormap/grass", &grass_map, error)) {
            return false;
        }

        // Materials: biome b (1-based) variant v is material (b - 1) * kVariants + v.
        std::vector<minecraft::Biome> vanilla(biome_count);
        std::vector<std::uint32_t> base_grass(biome_count, 0x91BD59);
        for (std::uint32_t b = 1; b < biome_count; ++b) {
            const BiomeRow& row = *biomes[b];
            std::array<std::uint32_t, 7> t{};
            for (std::size_t i = 0; i < t.size(); ++i) {
                if (!texture(row.blocks[i], &t[i])) {
                    return false;
                }
            }
            auto overlay_of = [&](const std::string& side, std::uint32_t* out) {
                *out = TerrainLook::kNone;
                return !assets.has_texture("block/" + side + "_overlay") || texture(side + "_overlay", out);
            };
            TerrainLook::Material own{ t[0], t[1], TerrainLook::kNone, t[2], t[3] };
            TerrainLook::Material shore{ t[4], t[4], TerrainLook::kNone, t[4], t[3] };
            TerrainLook::Material snow{ t[5], t[6], TerrainLook::kNone, t[2], t[3] };
            if (!overlay_of(row.blocks[1], &own.overlay) || !overlay_of(row.blocks[6], &snow.overlay)) {
                return false;
            }
            look->materials.push_back(own);
            look->materials.push_back(shore);
            look->materials.push_back(snow);
            if (!assets.biome(row.vanilla, &vanilla[b], error)) {
                *error = std::format("{}: biome {}: {}", biomes_csv.string(), row.key, *error);
                return false;
            }
            base_grass[b] = vanilla[b].grass_color.value_or(
                minecraft::colormap_color(grass_map, vanilla[b].temperature, vanilla[b].downfall));
        }

        // Per block: material and the colours to blend.
        const auto w = static_cast<int>(map.block_width);
        const auto h = static_cast<int>(map.block_height);
        const std::size_t blocks = static_cast<std::size_t>(w) * h;
        look->block_material.assign(blocks, 0);
        std::vector<std::uint32_t> grass(blocks, 0);
        std::vector<std::uint32_t> water(blocks, kDefaultWater);
        std::vector<std::uint8_t> dry(blocks, 0);
        std::vector<std::uint8_t> all(blocks, 1);
        for (int y = 0; y < h; ++y) {
            for (int x = 0; x < w; ++x) {
                const std::size_t i = static_cast<std::size_t>(y) * w + x;
                const BlockClass cls = map.block_class[i];
                const std::uint32_t b = std::min<std::uint32_t>(map.block_biome[i], biome_count - 1);
                const std::uint32_t biome = b == 0 ? 1 : b;
                if (b != 0) {
                    water[i] = vanilla[b].water_color;
                }
                grass[i] = grass_color(vanilla[biome], base_grass[biome], x, y);
                std::uint32_t variant = kOwn;
                if (cls == BlockClass::land || cls == BlockClass::wasteland) {
                    dry[i] = 1;
                    const int ground = map.block_ground[i];
                    const int snow_line = biomes[biome]->snow_line;
                    if (snow_line >= 0 && ground - kLandBase >= snow_line) {
                        variant = kSnow;
                    } else {
                        for (int dy = -1; dy <= 1 && variant == kOwn; ++dy) {
                            for (int dx = -1; dx <= 1; ++dx) {
                                const int xx = x + dx;
                                const int yy = y + dy;
                                if (xx < 0 || yy < 0 || xx >= w || yy >= h) {
                                    continue;
                                }
                                const std::size_t j = static_cast<std::size_t>(yy) * w + xx;
                                const BlockClass other = map.block_class[j];
                                if ((other == BlockClass::sea || other == BlockClass::lake) &&
                                    ground <= map.block_water[j] + 1) {
                                    variant = kShore;
                                    break;
                                }
                            }
                        }
                    }
                }
                look->block_material[i] = static_cast<std::uint8_t>((biome - 1) * kVariants + variant);
            }
        }
        look->grass_tint = blend(grass, dry, w, h);
        look->water_tint = blend(water, all, w, h);
        return true;
    }
}
