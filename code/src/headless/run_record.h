#pragma once

#include <cstdint>
#include <filesystem>
#include <span>
#include <string>

namespace mc
{
    class World;
}

namespace mc::headless
{
    struct Scenario;

    // What a run leaves in its folder (docs/SIMULATION.md, "Сейчас"), written at its end (made if missing):
    // meta.json, hashes.csv (one checksum per day from the start, `hashes[0]` the loaded state), state.csv and
    // checksum.txt of `end`. `root` is the repository, for the commit in meta.json. Monthly series come back with the
    // rules that have something to show (the war of M5, the treasury of M8).
    bool write_run(const std::filesystem::path& dir, const std::filesystem::path& root, const Scenario& scenario,
                   const World& end, std::span<const std::uint64_t> hashes, std::string* error);
}
