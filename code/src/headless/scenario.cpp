#include "headless/scenario.h"

#include <algorithm>
#include <charconv>
#include <format>
#include <fstream>
#include <string_view>
#include <vector>

#include "game/calendar.h"

namespace mc::headless
{
    namespace
    {
        std::string_view trim(std::string_view text)
        {
            const std::size_t begin = text.find_first_not_of(" \t\r");
            if (begin == std::string_view::npos) {
                return {};
            }
            const std::size_t end = text.find_last_not_of(" \t\r");
            return text.substr(begin, end - begin + 1);
        }

        template <class T>
        bool parse_whole(std::string_view text, T* out)
        {
            const auto [ptr, ec] = std::from_chars(text.data(), text.data() + text.size(), *out);
            return ec == std::errc{} && ptr == text.data() + text.size();
        }

        // The most years whose days still fit the day counter (u32).
        constexpr std::uint32_t kMaxYears = 10'000;

        constexpr std::string_view kBlank = " \t";

        // The words of a text, split at spaces and tabs.
        std::vector<std::string_view> words(std::string_view text)
        {
            std::vector<std::string_view> out;
            std::size_t i = 0;
            while (i < text.size()) {
                const std::size_t begin = text.find_first_not_of(kBlank, i);
                if (begin == std::string_view::npos) {
                    break;
                }
                const std::size_t end = std::min(text.find_first_of(kBlank, begin), text.size());
                out.push_back(text.substr(begin, end - begin));
                i = end;
            }
            return out;
        }

        // "TAG move slot:generation province" or "TAG war TAG".
        bool parse_command(std::string_view text, ScenarioCommand* out, std::string* why)
        {
            const std::vector<std::string_view> w = words(text);
            if (w.size() == 3 && w[1] == "war") {
                out->country = std::string(w[0]);
                out->target = std::string(w[2]);
                out->command.kind = CommandKind::declare_war;
                return true;
            }
            if (w.size() != 4 || w[1] != "move") {
                *why = "a command is TAG move slot:generation province, or TAG war TAG";
                return false;
            }
            const std::size_t colon = w[2].find(':');
            if (colon == std::string_view::npos ||
                !parse_whole(w[2].substr(0, colon), &out->command.army.slot) ||
                !parse_whole(w[2].substr(colon + 1), &out->command.army.generation) ||
                out->command.army.slot >= kArmySlots) {
                *why = std::format("army \"{}\": slot:generation, the slot below {}", w[2], kArmySlots);
                return false;
            }
            if (!parse_whole(w[3], &out->command.province) || out->command.province == 0) {
                *why = std::format("province \"{}\": an id of the map", w[3]);
                return false;
            }
            out->country = std::string(w[0]);
            out->command.kind = CommandKind::move_army;
            return true;
        }
    }

    bool load_scenario(const std::filesystem::path& path, Scenario* scenario, std::string* error)
    {
        *scenario = {};
        scenario->path = path;
        scenario->name = path.stem().string();
        std::ifstream in(path, std::ios::binary);
        if (!in) {
            *error = std::format("{}: cannot open", path.string());
            return false;
        }
        bool has_years = false;
        bool has_seed = false;
        std::string line;
        int number = 0;
        while (std::getline(in, line)) {
            ++number;
            const std::string origin = std::format("{}:{}", path.generic_string(), number);
            auto fail = [&](std::string message) {
                *error = std::format("{}: {}", origin, message);
                return false;
            };
            const std::string_view text = trim(line);
            if (text.empty() || text.starts_with('#')) {
                continue;
            }
            const std::size_t equals = text.find('=');
            if (equals == std::string_view::npos) {
                return fail("expected key = value");
            }
            const std::string_view key = trim(text.substr(0, equals));
            const std::string_view value = trim(text.substr(equals + 1));

            if (const std::size_t colon = key.find(':'); colon != std::string_view::npos) {
                // A data change: file: row.column = value.
                const std::string_view file = trim(key.substr(0, colon));
                const std::string_view field = trim(key.substr(colon + 1));
                const std::size_t dot = field.find('.');
                if (!file.ends_with(".csv") || dot == std::string_view::npos || dot == 0 || dot + 1 == field.size()) {
                    return fail("a data change is file.csv: row.column = value");
                }
                csv::Patch patch{ std::string(file), std::string(field.substr(0, dot)),
                                  std::string(field.substr(dot + 1)), std::string(value), origin };
                for (const csv::Patch& other : scenario->patches) {
                    if (other.file == patch.file && other.row == patch.row && other.column == patch.column) {
                        return fail(std::format("{}: {}.{} already changed at {}", patch.file, patch.row,
                                                patch.column, other.origin));
                    }
                }
                scenario->patches.push_back(std::move(patch));
            } else if (key.size() > 3 && key.starts_with("day") && kBlank.find(key[3]) != std::string_view::npos) {
                ScenarioCommand command;
                if (!parse_whole(trim(key.substr(4)), &command.day)) {
                    return fail(std::format("\"{}\": day and a whole number >= 0", key));
                }
                std::string why;
                if (!parse_command(value, &command, &why)) {
                    return fail(why);
                }
                command.origin = origin;
                scenario->commands.push_back(std::move(command));
            } else if (key == "years") {
                if (has_years) {
                    return fail("years repeated");
                }
                if (!parse_whole(value, &scenario->years) || scenario->years == 0 || scenario->years > kMaxYears) {
                    return fail(std::format("years \"{}\": a whole number from 1 to {}", value, kMaxYears));
                }
                has_years = true;
            } else if (key == "seed") {
                if (has_seed) {
                    return fail("seed repeated");
                }
                if (!parse_whole(value, &scenario->seed) || scenario->seed > 0xFFFFFFFFull) {
                    return fail(std::format("seed \"{}\": a whole number 0..4294967295", value));
                }
                has_seed = true;
            } else {
                return fail(std::format("unknown key \"{}\" (years, seed, day N, or file.csv: row.column)", key));
            }
        }
        if (!has_years || !has_seed) {
            *error = std::format("{}: needs years and seed", path.string());
            return false;
        }
        static_assert(std::uint64_t{ kMaxYears } * kDaysPerYear <= 0xFFFFFFFFu);
        for (const ScenarioCommand& command : scenario->commands) {
            if (command.day >= scenario->years * kDaysPerYear) {
                *error = std::format("{}: day {} is past the run's {} days", command.origin, command.day,
                                     scenario->years * kDaysPerYear);
                return false;
            }
        }
        std::stable_sort(scenario->commands.begin(), scenario->commands.end(),
                         [](const ScenarioCommand& a, const ScenarioCommand& b) { return a.day < b.day; });
        return true;
    }
}
