#include "core/map_data.h"

#include <algorithm>
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
