#include "core/map_data.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <format>
#include <fstream>

namespace mc
{
    namespace
    {
        struct Header
        {
            char magic[4];
            std::uint32_t version;
            std::uint32_t width;
            std::uint32_t height;
            std::uint32_t count;
            std::uint32_t sections;
        };
        static_assert(sizeof(Header) == 24);

        struct Section
        {
            char tag[4];
            std::uint32_t size;
            std::uint64_t offset;
        };
        static_assert(sizeof(Section) == 16);

        template <class T>
        std::span<const T> view(std::span<const std::byte> bytes)
        {
            return { reinterpret_cast<const T*>(bytes.data()), bytes.size() / sizeof(T) };
        }

        // String i of a table written by bake.py `strings`: u32 offsets (one more than strings) + UTF-8 blob.
        std::string_view string_at(std::span<const std::uint32_t> offsets, std::span<const char> blob, std::size_t i)
        {
            if (i + 1 >= offsets.size()) {
                return {};
            }
            const std::uint32_t begin = offsets[i];
            const std::uint32_t end = offsets[i + 1];
            if (begin > end || end > blob.size()) {
                return {};
            }
            return { blob.data() + begin, end - begin };
        }
    }

    bool MapData::load(const std::filesystem::path& path, std::string* error)
    {
        auto fail = [&](std::string message) {
            if (error) {
                *error = std::format("{}: {}", path.string(), message);
            }
            return false;
        };

        std::ifstream in(path, std::ios::binary | std::ios::ate);
        if (!in) {
            return fail("cannot open");
        }
        file_.resize(static_cast<std::size_t>(in.tellg()));
        in.seekg(0);
        in.read(reinterpret_cast<char*>(file_.data()), static_cast<std::streamsize>(file_.size()));
        if (!in || file_.size() < sizeof(Header)) {
            return fail("cannot read");
        }

        Header header;
        std::memcpy(&header, file_.data(), sizeof(header));
        if (std::memcmp(header.magic, "MCMP", 4) != 0 || header.version != 2) {
            return fail("not an MCMP v2 file (rebuild with tools/mapbake/bake.py)");
        }
        if (file_.size() < sizeof(Header) + std::size_t{ header.sections } * sizeof(Section)) {
            return fail("truncated section table");
        }
        width = header.width;
        height = header.height;
        count = header.count;

        auto section = [&](const char* tag) -> std::span<const std::byte> {
            for (std::uint32_t i = 0; i < header.sections; ++i) {
                Section s;
                std::memcpy(&s, file_.data() + sizeof(Header) + i * sizeof(Section), sizeof(s));
                if (std::memcmp(s.tag, tag, 4) == 0 && s.offset + s.size <= file_.size()) {
                    return { file_.data() + s.offset, s.size };
                }
            }
            return {};
        };

        const std::size_t pixels = std::size_t{ width } * height;
        pixel_province = view<std::uint16_t>(section("PIDM"));
        pixel_class = view<std::uint8_t>(section("CLAS"));
        kind = view<ProvinceKind>(section("PTYP"));
        region = view<std::uint16_t>(section("PREG"));
        color = view<std::uint32_t>(section("PCOL"));
        positions = view<float>(section("PPOS"));
        adjacency_offsets = view<std::uint32_t>(section("ADJO"));
        adjacency_ids = view<std::uint16_t>(section("ADJI"));

        const auto block_dims = view<std::uint32_t>(section("BDIM"));
        std::uint32_t biome_keys = 0;
        if (block_dims.size() >= 4) {
            block_width = block_dims[0];
            block_height = block_dims[1];
            block_size = block_dims[2];
            biome_keys = block_dims[3];
        }
        block_class = view<BlockClass>(section("BCLS"));
        block_ground = view<std::uint8_t>(section("BHGT"));
        block_water = view<std::uint8_t>(section("BWTR"));
        block_biome = view<std::uint8_t>(section("BBIO"));
        const auto biome_table = section("BKEY");
        const std::size_t biome_offsets = (std::size_t{ biome_keys } + 1) * sizeof(std::uint32_t);
        if (biome_keys > 0 && biome_table.size() >= biome_offsets) {
            biome_key_offsets_ = view<std::uint32_t>(biome_table.first(biome_offsets));
            biome_key_blob_ = view<char>(biome_table.subspan(biome_offsets));
            biome_count_ = biome_keys;
        }

        border_points = view<float>(section("BVTX"));
        border_chains = view<std::uint32_t>(section("BSEG"));
        river_points = view<float>(section("RVTX"));
        river_chains = view<std::uint32_t>(section("RSEG"));
        const auto font_params = view<std::int32_t>(section("FPAR"));
        if (font_params.size() >= 8) {
            font_line_height = font_params[0];
            font_base = font_params[1];
            font_padding = font_params[2];
            font_atlas_scale = font_params[3];
            font_extra = font_params[4];
            font_atlas_width = font_params[5];
            font_atlas_height = font_params[6];
            font_spread = font_params[7];
        }
        font_chars = view<FontChar>(section("FCHR"));
        font_kerning = view<FontKerning>(section("FKRN"));
        font_atlas = view<std::uint8_t>(section("FATL"));
        province_glyphs = view<float>(section("NGLY"));
        const auto region_names = section("RNAM");
        const std::size_t region_names_count = section("RPAR").size();   // u8 per region, [0] = none
        const std::size_t region_table = (region_names_count + 1) * sizeof(std::uint32_t);
        if (region_names_count > 0 && region_names.size() >= region_table) {
            region_name_offsets_ = view<std::uint32_t>(region_names.first(region_table));
            region_name_blob_ = view<char>(region_names.subspan(region_table));
        }
        // Lands: each region's parent, the names as RNAM, '' + one per land.
        region_parent = view<std::uint8_t>(section("RPAR"));
        for (const std::uint8_t l : region_parent) {
            land_count_ = std::max<std::uint32_t>(land_count_, l + 1u);
        }
        const auto land_names = section("LNAM");
        const std::size_t land_table = (std::size_t{ land_count_ } + 1) * sizeof(std::uint32_t);
        if (land_count_ > 0 && land_names.size() >= land_table) {
            land_name_offsets_ = view<std::uint32_t>(land_names.first(land_table));
            land_name_blob_ = view<char>(land_names.subspan(land_table));
        }
        const auto names = section("PNAM");
        const std::size_t name_table = (std::size_t{ count } + 1) * sizeof(std::uint32_t);
        if (names.size() >= name_table) {
            name_offsets_ = view<std::uint32_t>(names.first(name_table));
            name_blob_ = view<char>(names.subspan(name_table));
        }

        const std::size_t blocks = std::size_t{ block_width } * block_height;
        if (block_size == 0 || std::size_t{ block_width } * block_size != width ||
            std::size_t{ block_height } * block_size != height || block_class.size() != blocks ||
            block_ground.size() != blocks || block_water.size() != blocks || block_biome.size() != blocks ||
            biome_count_ == 0) {
            return fail("missing or malformed block sections (rebuild with tools/mapbake/bake.py)");
        }
        if (pixel_province.size() != pixels || pixel_class.size() != pixels || kind.size() != count ||
            color.size() != count || positions.size() != std::size_t{ count } * 8 || name_offsets_.empty() ||
            border_chains.size() % 4 != 0 || river_points.size() % 3 != 0 || river_chains.size() % 4 != 0 ||
            font_chars.empty() || font_line_height <= 0 || region_name_offsets_.empty() || land_name_offsets_.empty() ||
            province_glyphs.size() % 7 != 0 ||
            font_atlas.size() != std::size_t(font_atlas_width) * std::size_t(font_atlas_height)) {
            return fail("missing or malformed sections (rebuild with tools/mapbake/bake.py)");
        }
        for (const std::uint16_t r : region) {
            region_count_ = std::max<std::uint32_t>(region_count_, r + 1u);
        }
        return true;
    }

    std::string_view MapData::region_name(std::uint32_t index) const
    {
        return string_at(region_name_offsets_, region_name_blob_, index);
    }

    std::string_view MapData::land_name(std::uint32_t index) const
    {
        return string_at(land_name_offsets_, land_name_blob_, index);
    }

    std::string_view MapData::biome_key(std::uint32_t index) const
    {
        return string_at(biome_key_offsets_, biome_key_blob_, index);
    }

    std::string_view MapData::province_name(std::uint32_t id) const
    {
        return id < count ? string_at(name_offsets_, name_blob_, id) : std::string_view{};
    }
}
