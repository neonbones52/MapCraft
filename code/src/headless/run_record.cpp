#include "headless/run_record.h"

#include <chrono>
#include <format>
#include <fstream>
#include <sstream>
#include <string_view>

#include "game/calendar.h"
#include "game/world.h"
#include "headless/scenario.h"
#include "headless/snapshot.h"

namespace mc::headless
{
    namespace
    {
        std::string json_string(std::string_view text)
        {
            std::string out = "\"";
            for (const char c : text) {
                if (c == '"' || c == '\\') {
                    out += '\\';
                    out += c;
                } else if (static_cast<unsigned char>(c) < 0x20) {
                    out += std::format("\\u{:04x}", static_cast<unsigned>(static_cast<unsigned char>(c)));
                } else {
                    out += c;
                }
            }
            return out + "\"";
        }

        std::string read_text(const std::filesystem::path& path)
        {
            std::ifstream in(path, std::ios::binary);
            std::stringstream text;
            text << in.rdbuf();
            return text.str();
        }

        std::string_view first_line(std::string_view text)
        {
            const std::size_t end = text.find_first_of("\r\n");
            return end == std::string_view::npos ? text : text.substr(0, end);
        }

        // The commit the checkout stands on (.git/HEAD, then the ref or packed-refs); "unknown" if it cannot tell.
        // The exe may have been built from changes not committed yet.
        std::string commit(const std::filesystem::path& root)
        {
            const std::filesystem::path git = root / ".git";
            const std::string head_text = read_text(git / "HEAD");
            const std::string_view head = first_line(head_text);
            if (!head.starts_with("ref: ")) {
                return head.size() == 40 ? std::string(head) : "unknown";
            }
            const std::string_view ref = head.substr(5);
            const std::string loose = read_text(git / std::filesystem::path(ref));
            if (first_line(loose).size() == 40) {
                return std::string(first_line(loose));
            }
            std::istringstream packed(read_text(git / "packed-refs"));
            std::string line;
            while (std::getline(packed, line)) {
                const std::string_view l = first_line(line);
                if (l.size() == 41 + ref.size() && l.substr(41) == ref) {
                    return std::string(l.substr(0, 40));
                }
            }
            return "unknown";
        }

        bool write_file(const std::filesystem::path& path, std::string_view text, std::string* error)
        {
            std::ofstream out(path, std::ios::binary);
            out.write(text.data(), static_cast<std::streamsize>(text.size()));
            if (!out) {
                *error = std::format("{}: cannot write", path.string());
                return false;
            }
            return true;
        }
    }

    bool write_run(const std::filesystem::path& dir, const std::filesystem::path& root, const Scenario& scenario,
                   const World& end, std::span<const std::uint64_t> hashes, std::string_view moves,
                   std::string* error)
    {
        std::error_code made;
        std::filesystem::create_directories(dir, made);
        if (made) {
            *error = std::format("{}: cannot make the folder ({})", dir.string(), made.message());
            return false;
        }

        std::string changes;
        for (const csv::Patch& p : scenario.patches) {
            changes += std::format("{}\n    {}", changes.empty() ? "" : ",",
                                   json_string(std::format("{}: {}.{} = {}", p.file, p.row, p.column, p.value)));
        }
        const auto now = std::chrono::floor<std::chrono::seconds>(std::chrono::system_clock::now());
        const std::string meta = std::format(
            "{{\n"
            "  \"scenario\": {},\n"
            "  \"scenario_file\": {},\n"
            "  \"years\": {},\n"
            "  \"days\": {},\n"
            "  \"seed\": {},\n"
            "  \"changes\": [{}{}],\n"
            "  \"commands\": {},\n"
            "  \"commit\": {},\n"
            "  \"run_at\": \"{:%Y-%m-%dT%H:%M:%SZ}\",\n"
            "  \"checksum\": \"{:016x}\"\n"
            "}}\n",
            json_string(scenario.name), json_string(scenario.path.generic_string()), scenario.years,
            scenario.years * kDaysPerYear, scenario.seed, changes, changes.empty() ? "" : "\n  ", scenario.commands.size(),
            json_string(commit(root)), now, checksum(end));

        std::string days = "day,checksum\n";
        for (std::size_t d = 0; d < hashes.size(); ++d) {
            days += std::format("{},{:016x}\n", kStartDay + d, hashes[d]);
        }

        return write_file(dir / "meta.json", meta, error) && write_file(dir / "hashes.csv", days, error) &&
               write_file(dir / "moves.csv", moves, error) && write_file(dir / "state.csv", dump(end), error) &&
               write_file(dir / "checksum.txt", std::format("{:016x}\n", checksum(end)), error);
    }
}
