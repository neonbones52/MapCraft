#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

#include "core/csv.h"
#include "game/commands.h"

namespace mc::headless
{
    // A scenario of mapcraft_sim (docs/SIMULATION.md, "Headless-симулятор"): a text file of `key = value` lines,
    // `#` starting a comment line:
    //     years = 50                       how long it runs (360-day years)
    //     seed = 1                         the world's seed (nothing random in the rules yet: only recorded)
    //     common/goods.csv: iron.price = 3 a data change: in data/common/goods.csv, the row whose first column is
    //                                      iron, the column price, set to 3 before the file is parsed
    //     day 12 = DUM move 0:0 476        a command on day 12 of the run (0 = the start, before the first day
    //                                      passes): DUM orders the army in slot 0, generation 0, to march to
    //                                      province 476 (game/commands.h); commands of one day go in file order
    // years and seed are required; an unknown key, a repeated key or change, a command past the run's days, or a
    // malformed line fails with its line. The command's country is checked once the data is loaded (main.cpp).
    struct ScenarioCommand
    {
        std::uint32_t day = 0;     // of the run: 0 = the start
        std::string country;       // its tag
        Command command;           // the country still 0
        std::string origin;        // file:line
    };

    struct Scenario
    {
        std::filesystem::path path;
        std::string name;                      // the file's stem
        std::uint32_t years = 0;
        std::uint64_t seed = 0;
        std::vector<csv::Patch> patches;       // in the file's order
        std::vector<ScenarioCommand> commands; // by day, in the file's order within a day
    };

    bool load_scenario(const std::filesystem::path& path, Scenario* scenario, std::string* error);
}
