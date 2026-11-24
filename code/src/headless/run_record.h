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

    // What a run leaves in its folder (docs/SIMULATION.md, "Сейчас"): monthly series of prices, pops and money, the
    // checksum of every day, the end state and meta.json. Collected as the run goes, written at its end.
    class RunRecord
    {
    public:
        RunRecord();

        // One row per market and good (prices.csv), per pop type (pops.csv) and one of money (money.csv), for the
        // world as it is now: the run calls it at the start and after every month.
        void month(const World& world);

        // Writes the folder (made if missing): meta.json, prices.csv, pops.csv, money.csv, hashes.csv (one checksum per
        // day from the start, `hashes[0]` the loaded state), state.csv and checksum.txt of `end`. `root` is the
        // repository, for the commit in meta.json.
        bool write(const std::filesystem::path& dir, const std::filesystem::path& root, const Scenario& scenario,
                   const World& end, std::span<const std::uint64_t> hashes, std::string* error) const;

    private:
        std::string prices_;
        std::string pops_;
        std::string money_;
    };
}
