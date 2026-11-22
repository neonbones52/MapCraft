#pragma once

#include <string>
#include <vector>

#include "app/biome_table.h"
#include "render/models.h"
#include "render/terrain.h"

namespace mc
{
    class MapData;
    namespace minecraft
    {
        class Assets;
    }

    // What stands on the map (render/models.h), from the map's sites (PSIT), data/common/biomes.csv and the
    // player's Minecraft jar (docs/RENDER.md, "Модели на карте"):
    // - one house per land province on its 2 x 2 block site, facing its way: a village small house of the style
    //   of the biome there (`village`; snowy above the biome's snow line), picked per province;
    // - one log pier per coastal province, one block long from its coast block into the sea;
    // - trees: up to four per land block (a quarter-block cell each), `tree_density` of them on average, one of
    //   the biome's `trees` features (grown from the jar's configured features, several variants each), none
    //   at or above `tree_line`, on the shore, on house sites or pier blocks.
    // `look` gives the grass colour per block; foliage takes the vanilla biome's colour.
    bool build_scenery(const MapData& map, const std::vector<const BiomeRow*>& biomes, const TerrainLook& look,
                       const minecraft::Assets& assets, ModelSet* out, std::string* error);
}
