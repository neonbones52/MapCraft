#include "view/map_view.h"

#include <algorithm>
#include <cmath>
#include <format>

namespace mc
{
    bool MapView::load(const std::filesystem::path& path, std::string* error)
    {
        MapReader reader;
        if (!reader.open(path, &bytes_, error)) {
            return false;
        }
        read(reader);
        if (reader.ok()) {
            read_view(reader);
        }
        return reader.finish(error);
    }

    void MapView::read_view(MapReader& reader)
    {
        const std::size_t n = count;
        const std::size_t pixels = std::size_t{ width } * height;

        // Provinces: pixels, points, sites.
        pixel_province = as_span<std::uint16_t>(reader.exact("PIDM", pixels * 2));
        pixel_class = as_span<std::uint8_t>(reader.exact("CLAS", pixels));
        color = as_span<std::uint32_t>(reader.exact("PCOL", n * 4));
        positions = as_span<float>(reader.exact("PPOS", n * 8 * 4));
        sites = as_span<std::uint16_t>(reader.exact("PSIT", n * 6 * 2));

        // Blocks.
        const auto block_dims = as_span<std::uint32_t>(reader.exact("BDIM", 16));
        if (block_dims.size() == 4) {
            block_width = block_dims[0];
            block_height = block_dims[1];
            block_size = block_dims[2];
            biome_count_ = block_dims[3];
        }
        if (block_size == 0 || std::size_t{ block_width } * block_size != width ||
            std::size_t{ block_height } * block_size != height || biome_count_ == 0) {
            reader.bad(std::format("section BDIM: {}x{} blocks of {} px do not cover the {}x{} px map, or no biomes",
                                   block_width, block_height, block_size, width, height));
        }
        const std::size_t blocks = std::size_t{ block_width } * block_height;
        block_class = as_span<BlockClass>(reader.exact("BCLS", blocks));
        block_ground = as_span<std::uint8_t>(reader.exact("BHGT", blocks));
        block_water = as_span<std::uint8_t>(reader.exact("BWTR", blocks));
        block_biome = as_span<std::uint8_t>(reader.exact("BBIO", blocks));
        biome_keys_ = reader.strings("BKEY", biome_count_);

        // Borders and rivers (rivers may be empty: a map without rivers.png).
        border_points = as_span<float>(reader.whole("BVTX", 8, false));
        border_chains = as_span<std::uint32_t>(reader.whole("BSEG", 16, false));
        river_points = as_span<float>(reader.whole("RVTX", 12, true));
        river_chains = as_span<std::uint32_t>(reader.whole("RSEG", 16, true));

        // The names' font and the baked province names.
        const auto font_params = as_span<std::int32_t>(reader.exact("FPAR", 32));
        if (font_params.size() == 8) {
            font_line_height = font_params[0];
            font_base = font_params[1];
            font_padding = font_params[2];
            font_atlas_scale = font_params[3];
            font_extra = font_params[4];
            font_atlas_width = font_params[5];
            font_atlas_height = font_params[6];
            font_spread = font_params[7];
        }
        if (font_line_height <= 0 || font_atlas_width <= 0 || font_atlas_height <= 0) {
            reader.bad("section FPAR: no line height or atlas size");
        }
        font_chars = as_span<FontChar>(reader.whole("FCHR", sizeof(FontChar), false));
        font_kerning = as_span<FontKerning>(reader.whole("FKRN", sizeof(FontKerning), true));
        font_atlas = as_span<std::uint8_t>(reader.exact(
            "FATL", std::size_t(std::max(font_atlas_width, 0)) * std::size_t(std::max(font_atlas_height, 0))));
        province_glyphs = as_span<float>(reader.whole("NGLY", 7 * 4, true));
        province_glyph_owner = as_span<std::uint16_t>(reader.exact("NGLP", province_glyphs.size() / 7 * 2));
        if (!reader.ok()) {
            return;
        }

        // Values that index other arrays: ids within their tables, so nothing downstream reads out of bounds.
        auto below = [](auto values, std::size_t limit) {
            return std::ranges::all_of(values, [limit](auto v) { return std::size_t(v) < limit; });
        };
        if (!below(pixel_province, n)) {
            reader.bad("section PIDM: a pixel's province is past the province count");
        }
        if (!below(pixel_class, 4)) {
            reader.bad("section CLAS: unknown pixel class");
        }
        for (std::size_t id = 0; id < n; ++id) {
            const std::uint16_t* s = &sites[id * 6];
            const bool house = s[0] == kNoSite ||
                               (s[0] + 1u < block_width && s[1] + 1u < block_height && s[2] < 4);
            const bool pier = s[3] == kNoSite || (s[3] < block_width && s[4] < block_height && s[5] < 4);
            if (!house || !pier) {
                reader.bad(std::format("section PSIT: province {} has its house or pier off the block grid", id));
                break;
            }
        }
        if (!below(block_class, 5)) {
            reader.bad("section BCLS: unknown block class");
        }
        if (!below(block_biome, biome_count_)) {
            reader.bad("section BBIO: a block's biome is past the keys of BKEY");
        }
        const std::size_t border_point_count = border_points.size() / 2;
        for (std::size_t c = 0; c + 3 < border_chains.size(); c += 4) {
            const std::uint32_t pair = border_chains[c + 2];
            if (std::size_t{ border_chains[c] } + border_chains[c + 1] > border_point_count || (pair & 0xFFFF) >= n ||
                (pair >> 16) >= n) {
                reader.bad(std::format("section BSEG: chain {} reaches past BVTX or names a province past the count",
                                       c / 4));
                break;
            }
        }
        const std::size_t river_point_count = river_points.size() / 3;
        const std::size_t river_count = river_chains.size() / 4;
        for (std::size_t r = 0; r < river_count; ++r) {
            const std::uint32_t* chain = &river_chains[r * 4];
            if (std::size_t{ chain[0] } + chain[1] > river_point_count ||
                (chain[3] != 0xFFFFFFFFu && chain[3] >= river_count)) {
                reader.bad(std::format("section RSEG: river {} reaches past RVTX or flows into no river", r));
                break;
            }
        }
        auto cp1252 = [](std::int32_t code) { return code >= 0 && code < 256; };
        const bool chars = std::ranges::all_of(font_chars, [&](const FontChar& c) { return cp1252(c.code); });
        const bool kerning =
            std::ranges::all_of(font_kerning, [&](const FontKerning& k) { return cp1252(k.first) && cp1252(k.second); });
        if (!chars || !kerning) {
            reader.bad("section FCHR or FKRN: a character code outside cp1252");
        }
        for (std::size_t g = 0; g < province_glyph_owner.size(); ++g) {
            const float code = province_glyphs[g * 7 + 6];
            if (province_glyph_owner[g] >= n || !(code >= 0.0f && code < 256.0f) || code != std::floor(code)) {
                reader.bad(std::format(
                    "section NGLY or NGLP: glyph {} names a province past the count or is no cp1252 code", g));
                break;
            }
        }
    }

    std::string_view MapView::biome_key(std::uint32_t index) const
    {
        return biome_keys_.at(index);
    }
}
