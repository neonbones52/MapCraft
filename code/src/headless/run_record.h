#pragma once

#include <cstdint>
#include <filesystem>
#include <span>
#include <string>
#include <string_view>

namespace mc
{
    class World;
}

namespace mc::headless
{
    struct Scenario;

    // A series the run kept as it went (main.cpp): its file in the run's folder and its text.
    struct RunSeries
    {
        std::string_view file;
        std::string_view text;
    };

    // What a run leaves in its folder (docs/SIMULATION.md, "Сейчас"), written at its end (made if missing):
    // meta.json, hashes.csv (one checksum per day from the start, `hashes[0]` the loaded state), the `series`
    // (main.cpp: moves.csv, armies.csv, sieges.csv, control.csv), state.csv, cache.csv and checksum.txt of `end`.
    // `root` is the repository, for the commit in meta.json.
    bool write_run(const std::filesystem::path& dir, const std::filesystem::path& root, const Scenario& scenario,
                   const World& end, std::span<const std::uint64_t> hashes, std::span<const RunSeries> series,
                   std::string* error);
}
