#include "core/map_data.h"

#include <algorithm>
#include <cmath>
#include <format>

namespace mc
{
    bool MapData::load(const std::filesystem::path& path, std::string* error)
    {
        MapReader reader;
        if (!reader.open(path, &bytes_, error)) {
            return false;
        }
        read(reader);
        return reader.finish(error);
    }

    void MapData::read(MapReader& reader)
    {
        width = reader.width();
        height = reader.height();
        count = reader.count();
        const std::size_t n = count;

        kind = as_span<ProvinceKind>(reader.exact("PTYP", n));
        region = as_span<std::uint16_t>(reader.exact("PREG", n * 2));
        adjacency_offsets = as_span<std::uint32_t>(reader.exact("ADJO", (n + 1) * 4));
        adjacency_ids = as_span<std::uint16_t>(reader.whole("ADJI", 2, false));
        positions = as_span<float>(reader.exact("PPOS", n * 8 * 4));
        march_offsets = as_span<std::uint32_t>(reader.exact("MPTO", (adjacency_ids.size() + 1) * 4));
        march_points = as_span<float>(reader.whole("MPTV", 8, true));
        march_length = as_span<float>(reader.exact("MPTL", adjacency_ids.size() * 4));
        march_river = as_span<std::uint8_t>(reader.exact("MPTR", adjacency_ids.size()));
        names_ = reader.strings("PNAM", n);

        // Regions and lands: RPAR has a byte per region and [0]; the lands are what RPAR points to.
        region_parent = as_span<std::uint8_t>(reader.whole("RPAR", 1, false));
        region_count_ = static_cast<std::uint32_t>(region_parent.size());
        land_count_ = 0;
        for (const std::uint8_t l : region_parent) {
            land_count_ = std::max<std::uint32_t>(land_count_, l + 1u);
        }
        region_names_ = reader.strings("RNAM", region_count_);
        land_names_ = reader.strings("LNAM", land_count_);
        if (!reader.ok()) {
            return;
        }

        // Values that index other arrays: ids within their tables, so nothing downstream reads out of bounds.
        if (!std::ranges::all_of(kind, [](ProvinceKind k) { return static_cast<std::uint8_t>(k) < 5; })) {
            reader.bad("section PTYP: unknown province kind");
        }
        if (!std::ranges::all_of(region, [&](std::uint16_t r) { return r < region_count_; })) {
            reader.bad("section PREG: a province's region is past the regions of RPAR");
        }
        if (adjacency_offsets[0] != 0 || !std::ranges::is_sorted(adjacency_offsets) ||
            adjacency_offsets.back() != adjacency_ids.size()) {
            reader.bad("section ADJO: neighbour offsets out of order or not ending at ADJI's size");
        }
        if (!std::ranges::all_of(adjacency_ids, [n](std::uint16_t id) { return id != 0 && id < n; })) {
            reader.bad("section ADJI: a neighbour id is 0 or past the province count");
        }
        if (march_offsets[0] != 0 || !std::ranges::is_sorted(march_offsets) ||
            march_offsets.back() * 2 != march_points.size()) {
            reader.bad("section MPTO: march path offsets out of order or not ending at MPTV's points");
        }
        for (std::size_t k = 0; k < march_length.size() && reader.ok(); ++k) {
            const std::uint32_t points = march_offsets[k + 1] - march_offsets[k];
            if (!std::isfinite(march_length[k]) || march_length[k] < 0.0f || (points >= 2) != (march_length[k] > 0.0f) ||
                points == 1) {
                reader.bad("section MPTL: a march path's length does not fit its points");
            }
        }
        for (std::size_t k = 0; k < march_river.size() && reader.ok(); ++k) {
            if (march_river[k] > 1 || (march_river[k] == 1 && march_length[k] == 0.0f)) {
                reader.bad("section MPTR: a river crossing is not 0 or 1, or is not on a march path");
            }
        }
        if (!std::ranges::all_of(march_points, [](float v) { return std::isfinite(v); })) {
            reader.bad("section MPTV: a march path point is not a number");
        }
    }

    int MapData::adjacency_entry(std::uint32_t from, std::uint32_t to) const
    {
        if (from + 1 >= adjacency_offsets.size()) {
            return -1;
        }
        for (std::uint32_t k = adjacency_offsets[from]; k < adjacency_offsets[from + 1]; ++k) {
            if (adjacency_ids[k] == to) {
                return static_cast<int>(k);
            }
        }
        return -1;
    }

    std::span<const float> MapData::march_path(int entry) const
    {
        if (entry < 0 || static_cast<std::size_t>(entry) + 1 >= march_offsets.size()) {
            return {};
        }
        const std::uint32_t first = march_offsets[static_cast<std::size_t>(entry)];
        const std::uint32_t end = march_offsets[static_cast<std::size_t>(entry) + 1];
        return march_points.subspan(std::size_t{ first } * 2, std::size_t{ end - first } * 2);
    }

    std::string_view MapData::province_name(std::uint32_t id) const
    {
        return id < count ? names_.at(id) : std::string_view{};
    }

    std::string_view MapData::region_name(std::uint32_t index) const
    {
        return region_names_.at(index);
    }

    std::string_view MapData::land_name(std::uint32_t index) const
    {
        return land_names_.at(index);
    }
}
