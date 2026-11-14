#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace mc
{
    enum class ProvinceKind : std::uint8_t
    {
        none = 0,
        land = 1,
        sea = 2,
        lake = 3,
        wasteland = 4,   // land no one owns or crosses (EU4's wasteland)
    };

    enum class PixelClass : std::uint8_t
    {
        sea = 0,
        land = 1,
        lake = 2,
        wasteland = 3,
    };

    // The voxel surface per block (BCLS): as PixelClass, plus river water carved into the land.
    enum class BlockClass : std::uint8_t
    {
        sea = 0,
        land = 1,
        lake = 2,
        wasteland = 3,
        river = 4,
    };

    // Read-only view of build/map/world.mcmap (docs/MAPFORMAT.md). Arrays indexed by province id have
    // `count` entries, id 0 meaning "no province". All spans point into the loaded file.
    class MapData
    {
    public:
        bool load(const std::filesystem::path& path, std::string* error);

        std::uint32_t width = 0;
        std::uint32_t height = 0;
        std::uint32_t count = 0;

        std::span<const std::uint16_t> pixel_province;  // width * height
        std::span<const std::uint8_t> pixel_class;      // width * height, PixelClass
        std::span<const ProvinceKind> kind;
        std::span<const std::uint16_t> region;
        std::span<const std::uint8_t> region_parent;    // per region: its land (the region's parent), see land_name
        std::span<const std::uint32_t> color;           // RGBA8, debug colour
        std::span<const float> positions;               // count * 8: city, unit, text, port (x, y)
        std::span<const std::uint32_t> adjacency_offsets;
        std::span<const std::uint16_t> adjacency_ids;

        // Borders: polylines in map px; chain = first point, point count, a | b << 16 (a < b), flags (1 = closed).
        std::span<const float> border_points;       // x, y pairs
        std::span<const std::uint32_t> border_chains;  // 4 per chain

        // Voxel surface on the block grid (tools/mapgen/surface.py): a block is block_size x block_size map px.
        // Levels count blocks up from the sea floor: the sea's top is 1, land of height step s has its ground at
        // s + 2. Water top 0 means no water (dry land); a river's or lake's bed is one block under its water.
        std::uint32_t block_width = 0;
        std::uint32_t block_height = 0;
        std::uint32_t block_size = 0;
        std::span<const BlockClass> block_class;          // block_width * block_height
        std::span<const std::uint8_t> block_ground;       // ground top level
        std::span<const std::uint8_t> block_water;        // water top level, 0 = none
        std::span<const std::uint8_t> block_biome;        // index into biome_key, 0 on water
        [[nodiscard]] std::uint32_t biome_count() const { return biome_count_; }
        [[nodiscard]] std::string_view biome_key(std::uint32_t index) const;   // data/common/biomes.csv key, [0] = ""

        // Rivers (tools/mapbake/rivers.py): polylines from source to mouth, a point every map px. Point = x, y, width
        // (map px); river = first point, count, flags (1 = rises at a source on land, 2 = flows into another river,
        // 4 = into the sea or a lake), the river it flows into (~0u = none).
        std::span<const float> river_points;
        std::span<const std::uint32_t> river_chains;

        // Map label font (tools/fonts/mapfont.py): EU4 Mapfont.fnt-style metrics for the label placement and a signed
        // distance field atlas (u8, 0.5 = ink edge, font_spread texels across the range) for drawing.
        struct FontChar
        {
            std::int32_t code;        // cp1252 byte
            std::int32_t atlas_x;     // atlas cell: the metric quad at font_atlas_scale plus font_extra texels round it
            std::int32_t atlas_y;
            std::int32_t cell_width;
            std::int32_t cell_height;
            std::int32_t xoffset;     // Mapfont.fnt semantics: quad (with padding) against the cursor and line top
            std::int32_t yoffset;
            std::int32_t width;
            std::int32_t height;      // 0: no visible glyph (space)
            std::int32_t xadvance;
            std::int32_t quad_x;      // the metric (layout) quad's top-left in the cell, texels; the cell may hold more
            std::int32_t quad_y;      //   of the glyph than the quad (Q's tail)
        };
        struct FontKerning
        {
            std::int32_t first;
            std::int32_t second;
            std::int32_t amount;
        };
        std::int32_t font_line_height = 0;
        std::int32_t font_base = 0;
        std::int32_t font_padding = 0;
        std::int32_t font_atlas_scale = 1;
        std::int32_t font_extra = 0;
        std::int32_t font_atlas_width = 0;
        std::int32_t font_atlas_height = 0;
        std::int32_t font_spread = 1;
        std::span<const FontChar> font_chars;
        std::span<const FontKerning> font_kerning;
        std::span<const std::uint8_t> font_atlas;
        // Province names, baked (tools/labels/provinces.py): 7 floats per glyph, x, y (map px), reading direction x, y,
        // quad width, height (map px), cp1252 code.
        std::span<const float> province_glyphs;

        [[nodiscard]] std::string_view province_name(std::uint32_t id) const;
        // Map px points of a province (positions): its city, and its name's anchor.
        struct Point
        {
            float x = 0.0f;
            float y = 0.0f;
        };
        [[nodiscard]] Point city_position(std::uint32_t id) const { return position(id, 0); }
        [[nodiscard]] Point text_position(std::uint32_t id) const { return position(id, 2); }
        [[nodiscard]] std::uint32_t region_count() const { return region_count_; }
        [[nodiscard]] std::string_view region_name(std::uint32_t index) const;
        [[nodiscard]] std::uint32_t land_count() const { return land_count_; }
        [[nodiscard]] std::string_view land_name(std::uint32_t index) const;

    private:
        [[nodiscard]] Point position(std::uint32_t id, std::size_t slot) const
        {
            const std::size_t i = std::size_t{ id } * 8 + slot * 2;
            return i + 1 < positions.size() ? Point{ positions[i], positions[i + 1] } : Point{};
        }

        std::vector<std::byte> file_;
        std::span<const std::uint32_t> name_offsets_;
        std::span<const char> name_blob_;
        std::span<const std::uint32_t> region_name_offsets_;
        std::span<const char> region_name_blob_;
        std::uint32_t region_count_ = 0;
        std::span<const std::uint32_t> land_name_offsets_;
        std::span<const char> land_name_blob_;
        std::uint32_t land_count_ = 0;
        std::span<const std::uint32_t> biome_key_offsets_;
        std::span<const char> biome_key_blob_;
        std::uint32_t biome_count_ = 0;
    };
}
