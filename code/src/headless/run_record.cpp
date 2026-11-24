#include "headless/run_record.h"

#include <array>
#include <chrono>
#include <format>
#include <fstream>
#include <sstream>
#include <string_view>

#include "core/money.h"
#include "game/calendar.h"
#include "game/economy/ledger.h"
#include "game/world.h"
#include "headless/scenario.h"
#include "headless/snapshot.h"

namespace mc::headless
{
    namespace
    {
        // Grains as exact emeralds: "153999.998600".
        std::string emeralds(Money grains)
        {
            const char* sign = grains < 0 ? "-" : "";
            const std::uint64_t magnitude = grains < 0 ? 0 - static_cast<std::uint64_t>(grains)
                                                       : static_cast<std::uint64_t>(grains);
            const auto per = static_cast<std::uint64_t>(kGrainsPerEmerald);
            return std::format("{}{}.{:06}", sign, magnitude / per, magnitude % per);
        }

        // The row's leading columns: the day, and its date's year and month.
        std::string when(const World& world)
        {
            const Date date = world.date();
            return std::format("{},{},{}", world.day(), date.year, date.month);
        }

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

    RunRecord::RunRecord()
        : prices_("day,year,month,market,good,price\n"),
          pops_("day,year,month,pop_type,size,cash,essential,everyday,luxury\n"),
          money_("day,year,month,total,start,minted")
    {
        for (const std::string_view flow : kFlowKeys) {
            money_ += std::format(",{}", flow);
        }
        money_ += '\n';
    }

    void RunRecord::month(const World& world)
    {
        const Content& content = world.content();
        const WorldState& state = world.state();
        const std::string at = when(world);

        // Prices: every market and good but the money good (always 1).
        for (std::size_t m = 1; m < content.markets.size(); ++m) {
            for (std::size_t g = 1; g < content.goods.size(); ++g) {
                if (content.goods.key[g] != kMoneyGood) {
                    prices_ += std::format("{},{},{},{}\n", at, content.markets.key[m], content.goods.key[g],
                                           state.prices.at(g, m));
                }
            }
        }

        // Pops, per type over all provinces: people, cash, and how much of each tier they got on the last day
        // (weighted by people; empty when nobody of the type needs the tier).
        const Pops& pops = state.pops;
        for (std::size_t t = 1; t < pops.types && t < content.pop_types.size(); ++t) {
            double people = 0.0;
            Money cash = 0;
            std::array<double, kNeedTiers> got{};
            for (std::uint32_t p = 1; p < state.provinces.count; ++p) {
                const std::size_t a = t * pops.stride + p;
                people += pops.size[a];
                cash += state.money.cash[a];
                for (std::size_t tier = 0; tier < kNeedTiers; ++tier) {
                    got[tier] += static_cast<double>(pops.size[a]) * pops.filled[tier][a];
                }
            }
            pops_ += std::format("{},{},{:.0f},{}", at, content.pop_types.list.key[t], people, emeralds(cash));
            for (std::size_t tier = 0; tier < kNeedTiers; ++tier) {
                if (people > 0.0 && content.pop_types.need[t][tier] > 0.0f) {
                    pops_ += std::format(",{:.6f}", got[tier] / people);
                } else {
                    pops_ += ',';
                }
            }
            pops_ += '\n';
        }

        // Money: the ledger's totals and its counter per flow, since the start.
        const Balances& money = state.money;
        money_ += std::format("{},{},{},{}", at, emeralds(ledger::total(money)), emeralds(money.start),
                              emeralds(money.minted));
        for (const Money flow : money.flows) {
            money_ += std::format(",{}", emeralds(flow));
        }
        money_ += '\n';
    }

    bool RunRecord::write(const std::filesystem::path& dir, const std::filesystem::path& root,
                          const Scenario& scenario, const World& end, std::span<const std::uint64_t> hashes,
                          std::string* error) const
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
            "  \"commit\": {},\n"
            "  \"run_at\": \"{:%Y-%m-%dT%H:%M:%SZ}\",\n"
            "  \"checksum\": \"{:016x}\"\n"
            "}}\n",
            json_string(scenario.name), json_string(scenario.path.generic_string()), scenario.years,
            scenario.years * kDaysPerYear, scenario.seed, changes, changes.empty() ? "" : "\n  ",
            json_string(commit(root)), now, checksum(end));

        std::string days = "day,checksum\n";
        for (std::size_t d = 0; d < hashes.size(); ++d) {
            days += std::format("{},{:016x}\n", kStartDay + d, hashes[d]);
        }

        return write_file(dir / "meta.json", meta, error) && write_file(dir / "prices.csv", prices_, error) &&
               write_file(dir / "pops.csv", pops_, error) && write_file(dir / "money.csv", money_, error) &&
               write_file(dir / "hashes.csv", days, error) && write_file(dir / "state.csv", dump(end), error) &&
               write_file(dir / "checksum.txt", std::format("{:016x}\n", checksum(end)), error);
    }
}
