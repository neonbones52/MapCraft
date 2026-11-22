#pragma once

#include <array>
#include <filesystem>
#include <string>
#include <vector>

namespace mc
{
    class MapData;

    // A row of data/common/biomes.csv (docs/GAMEDATA.md): what a map biome looks like.
    struct BiomeRow
    {
        std::string key;
        std::string vanilla;                 // minecraft_biome
        std::array<std::string, 7> blocks;   // top, side, under, deep, shore, snow_top, snow_side
        int snow_line = -1;                  // height step, -1 = never
        std::vector<std::string> trees;      // configured features, one picked per tree (repeats weigh more)
        float tree_density = 0.0f;           // trees per map block (0..1)
        int tree_line = -1;                  // height step from which no trees grow, -1 = none
        std::string village;                 // village style of its houses: plains, desert, savanna, taiga, snowy
    };

    bool read_biome_table(const std::filesystem::path& path, std::vector<BiomeRow>* rows, std::string* error);

    // The map's biomes (BKEY order, [0] = null) as rows of the table; an error names a biome the table lacks.
    bool map_biome_rows(const MapData& map, const std::vector<BiomeRow>& table, const std::filesystem::path& path,
                        std::vector<const BiomeRow*>* out, std::string* error);
}
