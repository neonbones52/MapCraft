#pragma once

#include <cstdint>
#include <vector>

#include "view/labels.h"
#include "render/map_renderer.h"

namespace mc
{
    class MapView;
    class GameData;

    enum class MapMode : std::uint32_t
    {
        terrain = 0,        // relief and material only
        political = 1,      // owners (data/setup); the regions while there is no game data
        provinces = 2,      // every province in its own debug colour
        regions = 3,        // the map's regions (holds, counties), no capitals
        terrain_type = 4,   // logical terrain (data/common/terrain.csv), not the look of the map
        rgo = 5,            // the RGO type (data/common/rgo_types.csv)
        markets = 6,        // the market of each province's region (data/common/markets.csv, data/setup/regions.csv)
        prices = 7,         // one good's price on each market against its start: blue cheaper, grey even, red dearer
        unfilled = 8,       // editing aid: red where owner, controller, terrain or RGO is missing
    };
    inline constexpr std::uint32_t kMapModeCount = 9;
    [[nodiscard]] const char* map_mode_name(MapMode mode);

    // What a map mode paints: one fill per province and one border style per border chain. Country borders run
    // between different owners in every mode, as in EU4 (between regions in the regions mode, and while there is no
    // game data). `game` may be null or not loaded. `price_good`: the good the prices mode shows (Content::goods).
    void map_mode_colors(const MapView& map, const GameData* game, MapMode mode, std::uint8_t price_good,
                         std::vector<ProvinceFill>* fills, std::vector<BorderStyle>* styles);

    // The map names a mode shows (EU4's SetMapNameMode): the countries (with their capitals) in the terrain and
    // political modes, the regions in the regions mode and without game data, the markets in theirs and in the prices
    // mode; none in the others. All are EU4's
    // realm names (realm_records), the same grouping across water in every mode: a sea or lake whose shores all have
    // one key (owner, region, market) joins that key's group when it touches at least two of its pieces. EU4 needs
    // pieces of two provinces; ours bridge single provinces too, so a capital's isle, coastal islets and a region's
    // islands are named with the mainland (the name runs over the water), not as tiny copies.
    std::vector<MapLabel> map_mode_labels(const MapView& map, const GameData* game, const LabelRaster& raster,
                                          const LabelFont& font, MapMode mode);
    // The key (owner, region, market) a mode groups each province's name by, 0 = none: blocks of a name's own group
    // never hide it (render/labels.h).
    std::vector<std::uint32_t> map_mode_label_keys(const MapView& map, const GameData* game, MapMode mode);
}
