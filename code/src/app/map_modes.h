#pragma once

#include <cstdint>
#include <vector>

#include "core/labels.h"
#include "render/map_renderer.h"

namespace mc
{
    class MapData;
    class GameData;

    enum class MapMode : std::uint32_t
    {
        terrain = 0,        // relief and material only
        political = 1,      // owners (data/setup); the regions while there is no game data
        provinces = 2,      // every province in its own debug colour
        regions = 3,        // the map's regions (holds, counties), no capitals
        terrain_type = 4,   // logical terrain (data/common/terrain.csv), not the look of the map
        culture = 5,
        religion = 6,
        development = 7,    // tax + production + manpower, red (least) to green (most)
        trade_goods = 8,
        unfilled = 9,       // editing aid: red where owner, controller, terrain, culture, religion or development is missing
    };
    inline constexpr std::uint32_t kMapModeCount = 10;
    [[nodiscard]] const char* map_mode_name(MapMode mode);

    // What a map mode paints: one fill per province and one border style per border chain. Country borders run
    // between different owners in every mode, as in EU4 (between regions in the regions mode, and while there is no
    // game data). `game` may be null or not loaded.
    void map_mode_colors(const MapData& map, const GameData* game, MapMode mode, std::vector<ProvinceFill>* fills,
                         std::vector<BorderStyle>* styles);

    // The map names a mode shows (EU4's SetMapNameMode): the countries (with their capitals) in the terrain and
    // political modes, the regions in the regions mode and without game data, the cultures and religions in theirs;
    // none in the others. All are EU4's realm names (realm_records), the same grouping across water in every mode:
    // a sea or lake whose shores all have one key (owner, region, culture, ...) joins that key's group when it touches
    // at least two of its pieces. EU4 needs pieces of two provinces; ours bridge single provinces too, so a capital's
    // isle, coastal islets and a culture's islands are named with the mainland (the name runs over the water),
    // not as tiny copies.
    std::vector<MapLabel> map_mode_labels(const MapData& map, const GameData* game, const LabelRaster& raster,
                                          const LabelFont& font, MapMode mode);
}
