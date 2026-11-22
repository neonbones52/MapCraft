#include "app/biome_table.h"

#include <algorithm>
#include <charconv>
#include <format>

#include "core/csv.h"
#include "core/map_data.h"

namespace mc
{
    namespace
    {
        // An optional whole number: "" -> -1.
        bool step(const std::string& text, int* out)
        {
            if (text.empty()) {
                *out = -1;
                return true;
            }
            const auto [end, ec] = std::from_chars(text.data(), text.data() + text.size(), *out);
            return ec == std::errc() && end == text.data() + text.size() && *out >= 0;
        }
    }

    bool read_biome_table(const std::filesystem::path& path, std::vector<BiomeRow>* rows, std::string* error)
    {
        csv::Table table;
        if (!csv::read(path, &table, error)) {
            return false;
        }
        const std::array<const char*, 14> names = { "key",       "minecraft_biome", "top",          "side",
                                                    "under",     "deep",            "shore",        "snow_top",
                                                    "snow_side", "snow_line",       "trees",        "tree_density",
                                                    "tree_line", "village" };
        std::array<int, names.size()> col{};
        for (std::size_t c = 0; c < names.size(); ++c) {
            col[c] = table.column(names[c]);
            if (col[c] < 0) {
                *error = std::format("{}: no column {}", path.string(), names[c]);
                return false;
            }
        }
        rows->clear();
        for (std::size_t r = 0; r < table.rows.size(); ++r) {
            const auto& row = table.rows[r];
            auto bad = [&](const char* column, const std::string& value, const char* what) {
                *error = std::format("{}:{}: {} '{}' is not {}", path.string(), table.lines[r], column, value, what);
                return false;
            };
            BiomeRow biome;
            biome.key = row[col[0]];
            biome.vanilla = row[col[1]];
            if (!step(row[col[9]], &biome.snow_line)) {
                return bad("snow_line", row[col[9]], "a height step");
            }
            if (!step(row[col[12]], &biome.tree_line)) {
                return bad("tree_line", row[col[12]], "a height step");
            }
            // Snow blocks are needed only with a snow line; without one they stay the biome's own.
            for (std::size_t b = 0; b < biome.blocks.size(); ++b) {
                biome.blocks[b] = row[col[2 + b]];
                const bool snow = b >= 5;
                if (biome.blocks[b].empty() && snow && biome.snow_line < 0) {
                    biome.blocks[b] = biome.blocks[b - 5];
                }
                if (biome.blocks[b].empty()) {
                    *error = std::format("{}:{}: {} has no {}", path.string(), table.lines[r], biome.key,
                                         names[2 + b]);
                    return false;
                }
            }
            if (biome.vanilla.empty()) {
                *error = std::format("{}:{}: {} has no minecraft_biome", path.string(), table.lines[r], biome.key);
                return false;
            }
            const std::string& trees = row[col[10]];
            for (std::size_t at = 0; at < trees.size();) {
                const std::size_t space = std::min(trees.find(' ', at), trees.size());
                if (space > at) {
                    biome.trees.push_back(trees.substr(at, space - at));
                }
                at = space + 1;
            }
            const std::string& density = row[col[11]];
            if (!density.empty()) {
                const auto [end, ec] =
                    std::from_chars(density.data(), density.data() + density.size(), biome.tree_density);
                if (ec != std::errc() || end != density.data() + density.size() || biome.tree_density < 0.0f ||
                    biome.tree_density > 1.0f) {
                    return bad("tree_density", density, "a number from 0 to 1");
                }
            }
            biome.village = row[col[13]];
            constexpr std::array<std::string_view, 5> styles = { "plains", "desert", "savanna", "taiga", "snowy" };
            if (std::ranges::find(styles, biome.village) == styles.end()) {
                return bad("village", biome.village, "plains, desert, savanna, taiga or snowy");
            }
            rows->push_back(std::move(biome));
        }
        return true;
    }

    bool map_biome_rows(const MapData& map, const std::vector<BiomeRow>& table, const std::filesystem::path& path,
                        std::vector<const BiomeRow*>* out, std::string* error)
    {
        const std::uint32_t count = map.biome_count();
        out->assign(count, nullptr);
        for (std::uint32_t b = 1; b < count; ++b) {
            const std::string_view key = map.biome_key(b);
            const auto row = std::ranges::find(table, key, &BiomeRow::key);
            if (row == table.end()) {
                *error = std::format("{}: no biome {} (the map was baked with it; rebake or add it)", path.string(),
                                     key);
                return false;
            }
            (*out)[b] = &*row;
        }
        return true;
    }
}
