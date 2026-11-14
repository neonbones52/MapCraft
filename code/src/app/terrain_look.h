#pragma once

#include <filesystem>
#include <string>

#include "render/terrain.h"

namespace mc
{
    class MapData;
    namespace minecraft
    {
        class Assets;
    }

    // What the blocks look like (render/terrain.h), from data/common/biomes.csv and the player's Minecraft jar:
    // - per map biome its blocks (top, side, under, deep; shore and snow variants) and the vanilla biome whose
    //   temperature, downfall and colours tint its grass and water (colour maps grass.png, swamp and dark forest
    //   modifiers as in Minecraft);
    // - per block its material: snow at and above the biome's snow line (height step), shore on land one block above
    //   the sea or a lake next to it (8 neighbours), else the biome's own;
    // - tints blended over 5 x 5 blocks (Minecraft's default biome blend).
    // Textures tinted by the grass colour: grass_block_top and every *_overlay; by the water colour: water_*.
    bool build_terrain_look(const MapData& map, const std::filesystem::path& biomes_csv,
                            const minecraft::Assets& assets, TerrainLook* look, std::string* error);
}
