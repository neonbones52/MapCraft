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

    // What a run leaves in its folder (docs/SIMULATION.md, "Сейчас"), written at its end (made if missing):
    // meta.json, hashes.csv (one checksum per day from the start, `hashes[0]` the loaded state), moves.csv (`moves`:
    // the steps the armies ended, main.cpp), armies.csv (`armies`: every army at the start and at each month's start,
    // main.cpp), state.csv, cache.csv and checksum.txt of `end`. `root` is the repository, for the commit in
    // meta.json. More monthly series come with the rules that have something to show per month (the war of M7, the
    // treasury of M8).
    bool write_run(const std::filesystem::path& dir, const std::filesystem::path& root, const Scenario& scenario,
                   const World& end, std::span<const std::uint64_t> hashes, std::string_view moves,
                   std::string_view armies, std::string* error);
}
