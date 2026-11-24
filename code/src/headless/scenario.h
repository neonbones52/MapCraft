#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

#include "core/csv.h"

namespace mc::headless
{
    // A scenario of mapcraft_sim (docs/SIMULATION.md, "Headless-симулятор"): a text file of `key = value` lines,
    // `#` starting a comment line:
    //     years = 50                       how long it runs (360-day years)
    //     seed = 1                         the world's seed (nothing random in the rules yet: only recorded)
    //     common/goods.csv: iron.price = 3 a data change: in data/common/goods.csv, the row whose first column is
    //                                      iron, the column price, set to 3 before the file is parsed
    // years and seed are required; an unknown key, a repeated key or change, or a malformed line fails with its line.
    struct Scenario
    {
        std::filesystem::path path;
        std::string name;                      // the file's stem
        std::uint32_t years = 0;
        std::uint64_t seed = 0;
        std::vector<csv::Patch> patches;       // in the file's order
    };

    bool load_scenario(const std::filesystem::path& path, Scenario* scenario, std::string* error);
}
