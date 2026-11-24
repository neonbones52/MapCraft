// mapcraft_sim.exe (docs/SIMULATION.md, "Headless-симулятор"): the simulation with no window, linked on mc_sim alone.
//
//     mapcraft_sim <days> <run dir>
//
// Loads the map's simulation part and data/ twice, into two worlds of their own, runs each `days` days through
// World::advance_days, and checks that both end bit for bit the same (the state's checksum: here and only here,
// CLAUDE.md, rule 11) and that the money invariant held every day. Writes the first world's saved state
// (game/saved_state.h) to <run dir>/state.csv and its checksum to <run dir>/checksum.txt.
// Exit codes: 0 all held, 1 bad arguments or a load or write failed, 2 the money invariant broke, 3 the runs differ.

#include <charconv>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <format>
#include <fstream>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <type_traits>

#include "core/log.h"
#include "core/map_data.h"
#include "game/calendar.h"
#include "game/saved_state.h"
#include "game/world.h"

namespace
{
    constexpr int kOk = 0;
    constexpr int kFailed = 1;
    constexpr int kUnbalanced = 2;
    constexpr int kDiverged = 3;

    void to_stderr(mc::log::Level level, std::string_view message)
    {
        const char* tag = level == mc::log::Level::error ? "error: " : level == mc::log::Level::warn ? "warn: " : "";
        std::fprintf(stderr, "%s%.*s\n", tag, static_cast<int>(message.size()), message.data());
    }

    // FNV-1a, 64 bits: over each saved column's name, length and raw bytes in for_each_saved's order, then the day.
    class Checksum
    {
    public:
        void add(std::span<const std::byte> bytes)
        {
            for (const std::byte b : bytes) {
                hash_ = (hash_ ^ static_cast<std::uint64_t>(b)) * 0x100000001b3ull;
            }
        }
        template <class T>
        void add_value(const T& value)
        {
            add(std::as_bytes(std::span(&value, 1)));
        }
        [[nodiscard]] std::uint64_t value() const { return hash_; }

    private:
        std::uint64_t hash_ = 0xcbf29ce484222325ull;
    };

    std::uint64_t checksum(const mc::World& world)
    {
        Checksum sum;
        mc::for_each_saved(world.state(), [&](std::string_view name, auto column) {
            sum.add(std::as_bytes(std::span(name)));
            sum.add_value(static_cast<std::uint64_t>(column.size()));
            sum.add(std::as_bytes(column));
        });
        sum.add_value(world.day());
        return sum.value();
    }

    // Every saved column, element by element (padding included, as the checksum sees it): column,index,value. Floats
    // print in their shortest exact form, so the dump reads back to the same bits.
    std::string dump(const mc::World& world)
    {
        std::string out = "column,index,value\n";
        out += std::format("day,0,{}\n", world.day());
        mc::for_each_saved(world.state(), [&](std::string_view name, auto column) {
            for (std::size_t i = 0; i < column.size(); ++i) {
                using T = std::remove_cv_t<typename decltype(column)::element_type>;
                if constexpr (std::is_floating_point_v<T>) {
                    out += std::format("{},{},{}\n", name, i, column[i]);
                } else {
                    out += std::format("{},{},{}\n", name, i, static_cast<std::uint64_t>(column[i]));
                }
            }
        });
        return out;
    }

    bool write_file(const std::filesystem::path& path, std::string_view text)
    {
        std::ofstream out(path, std::ios::binary);
        out.write(text.data(), static_cast<std::streamsize>(text.size()));
        if (!out) {
            mc::log::error("cannot write {}", path.string());
            return false;
        }
        return true;
    }

    // One world from data/, `days` days on; null if it does not load.
    std::unique_ptr<mc::World> run(const mc::MapData& map, std::uint32_t days, double* seconds)
    {
        auto world = std::make_unique<mc::World>();
        std::string error;
        if (!world->load(MC_DEFAULT_DATA, map, &error)) {
            mc::log::error("{}", error);
            return nullptr;
        }
        const auto begin = std::chrono::steady_clock::now();
        world->advance_days(days);
        *seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - begin).count();
        return world;
    }
}

int main(int argc, char** argv)
{
    mc::log::set_sink(&to_stderr);
    std::uint32_t days = 0;
    const std::string_view days_text = argc > 1 ? argv[1] : "";
    const auto parsed = std::from_chars(days_text.data(), days_text.data() + days_text.size(), days);
    if (argc != 3 || parsed.ec != std::errc{} || parsed.ptr != days_text.data() + days_text.size()) {
        std::fprintf(stderr, "usage: mapcraft_sim <days> <run dir>\n");
        return kFailed;
    }
    const std::filesystem::path run_dir = argv[2];

    mc::MapData map;
    std::string error;
    if (!map.load(MC_DEFAULT_MAP, &error)) {
        mc::log::error("{}", error);
        return kFailed;
    }

    double seconds_a = 0.0;
    double seconds_b = 0.0;
    const auto a = run(map, days, &seconds_a);
    const auto b = a ? run(map, days, &seconds_b) : nullptr;
    if (!a || !b) {
        return kFailed;
    }
    const std::uint64_t sum_a = checksum(*a);
    const std::uint64_t sum_b = checksum(*b);
    const double years = static_cast<double>(days) / mc::kDaysPerYear;
    mc::log::info("{} days ({:.1f} years) twice: {:.3f} s and {:.3f} s, {:.4f} s per year", days, years, seconds_a,
                  seconds_b, years > 0.0 ? (seconds_a + seconds_b) / 2.0 / years : 0.0);

    std::error_code made;
    std::filesystem::create_directories(run_dir, made);
    if (made) {
        mc::log::error("cannot make {}: {}", run_dir.string(), made.message());
        return kFailed;
    }
    const std::string sum_text = std::format("{:016x}\n", sum_a);
    if (!write_file(run_dir / "state.csv", dump(*a)) || !write_file(run_dir / "checksum.txt", sum_text)) {
        return kFailed;
    }
    mc::log::info("checksum {:016x}, state in {}", sum_a, (run_dir / "state.csv").string());

    if (!a->money_balanced() || !b->money_balanced()) {
        mc::log::error("the money invariant broke (the first day is logged above)");
        return kUnbalanced;
    }
    if (sum_a != sum_b) {
        mc::log::error("the two runs differ: checksum {:016x} and {:016x}", sum_a, sum_b);
        return kDiverged;
    }
    mc::log::info("both runs match bit for bit; the money invariant held every day");
    return kOk;
}
