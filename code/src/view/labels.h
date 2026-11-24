#pragma once

#include <array>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

namespace mc
{
    class MapData;

    // EU4's automatic map names, ported one to one from the reverse engineering in archeologyEU4/Labels (LABEL.md,
    // docs/label/*.md) and its verified Python port (LabelPython): province group -> pixel moments -> two OLS
    // regressions -> ValidateLine -> size and letter spacing -> cross-sections -> two Catmull-Rom segments -> glyphs.
    //
    // All placement runs in EU4's raster convention: integer pixel coordinates without +0.5, y = 0 the BOTTOM row
    // (EU4 reads provinces.bmp bottom-up). Results are converted to our map px (y down, pixel corners on integers)
    // only at the end, in MapLabel.

    // DoPostRead + CalculateAdjacencies once per map: the province raster bottom-up, per province the six moments
    // (N, Sx, Sy, Sxy, Sxx, Syy; int32 products, uint64 sums), the half-open bbox, the ordered neighbour lists, and
    // land/lake flags.
    class LabelRaster
    {
    public:
        void build(const MapData& map);

        std::int32_t width = 0;
        std::int32_t height = 0;
        std::vector<std::uint16_t> pixels;                       // [y * width + x], y = 0 bottom
        std::vector<std::array<std::uint64_t, 6>> moments;       // per province
        std::vector<std::array<std::int32_t, 4>> bbox;           // xmin, ymin, xmax (exclusive), ymax (exclusive)
        std::vector<std::vector<std::uint16_t>> neighbours;      // CalculateAdjacencies order
        std::vector<std::uint8_t> land;
        std::vector<std::uint8_t> lake;
        std::vector<std::uint16_t> used;                         // _UsedProvinces: ids with pixels, ascending

        [[nodiscard]] std::size_t count() const { return land.size(); }
    };

    // Mapfont.fnt metrics (MapData::font_*) with byte lookup; measure() is GetActualRequiredSize for one line.
    class LabelFont
    {
    public:
        void build(const MapData& map);
        struct Char
        {
            bool present = false;
            std::int32_t xoffset = 0, yoffset = 0, width = 0, height = 0, xadvance = 0;
            std::int32_t atlas_x = 0, atlas_y = 0, cell_width = 0, cell_height = 0, quad_x = 0, quad_y = 0;
        };
        [[nodiscard]] std::int32_t measure(const std::string& text) const;
        [[nodiscard]] std::int32_t kerning(std::uint8_t a, std::uint8_t b) const;

        std::int32_t line_height = 100;
        std::int32_t padding = 8;
        std::array<Char, 256> chars{};

    private:
        std::vector<std::int32_t> kerning_;                      // 256 * 256
    };

    // One SProvinceData record: a province, its AreaID, the group's name (cp1252, at most 45 bytes) and the two
    // water flags of that province (_bAllowWaterBetweenBlob = isLand, _bAllowWaterEnds = !isLand).
    struct NameRecord
    {
        std::uint16_t province = 0;
        std::int32_t area = 0;
        std::string name;
        bool water_between = false;
        bool water_ends = false;
    };

    // MAPNAME_REALMS input (SetMapNameMode, docs/label/groups.md §4-5). Countries are indices; any "country" works:
    // owners, and regions, cultures and religions as capital-less countries (app/map_modes.cpp).
    struct RealmNames
    {
        std::vector<std::int32_t> owner;          // per province: country or -1
        std::vector<std::int32_t> tribal_owner;   // per province: country or -1
        std::vector<std::uint8_t> impassable;     // per province
        std::vector<std::uint16_t> capital;       // per country: province id, 0 = none
        std::vector<std::string> name;            // per country, UTF-8
        std::vector<std::string> adjective;       // per country, UTF-8
        std::vector<std::uint8_t> colonial;       // per country
        // Geography for the names of remote parts: per province an index or -1; names per index.
        std::vector<std::int32_t> area;
        std::vector<std::int32_t> region;
        std::vector<std::int32_t> superregion;
        std::vector<std::string> area_names;
        std::vector<std::string> region_names;
        std::vector<std::string> superregion_names;
        bool color_wastelands = false;            // SETTING_COLOR_WASTELANDS
        bool use_region_names = true;             // SETTING_USE_REGION_NAMES
        std::int32_t min_provinces_for_name = 2;  // NDefines.NGraphics.MIN_PROVINCES_FOR_NAME_ON_MAP
        std::int32_t min_provinces_for_region = 4;  // MIN_PROVINCES_FOR_REGION
        // GetRealmIdentifier: a water province whose land neighbours are all one owner takes that owner when it
        // touches at least two of its pieces this large (EU4: more than one province; ours: one, app/map_modes.cpp).
        std::int32_t min_bridged_piece = 2;
        std::string enclave_format = "$ADJECTIVE$ $AREANAME$";   // ENCLAVE_NAME_FORMAT
    };

    std::vector<NameRecord> realm_records(const LabelRaster& raster, const RealmNames& realms);

    // A placed glyph in OUR map px (x right, y down): the centre of its quad, the unit direction of the text there
    // (reading direction), the quad's size along and across it (map px, the Mapfont quad with padding), the byte.
    struct LabelGlyph
    {
        float x = 0.0f;
        float y = 0.0f;
        float dir_x = 1.0f;
        float dir_y = 0.0f;
        float width = 0.0f;
        float height = 0.0f;
        std::uint8_t code = 0;
    };

    struct MapLabel
    {
        std::string text;          // the measured, upper-cased and spaced string (cp1252)
        float scale = 0.0f;        // map px per font px
        std::vector<LabelGlyph> glyphs;
    };

    // CGenerateNamesWork::Update: one attempt per group, in record order; a failed attempt also consumes its group.
    std::vector<MapLabel> generate_names(const LabelRaster& raster, const LabelFont& font,
                                         std::span<const NameRecord> records);

    // UTF-8 -> cp1252 (unknown characters become '?'), and EU4's byte-wise StrToUpper on cp1252.
    std::string to_cp1252(const std::string& utf8);
}
