#pragma once

#include <cstdint>
#include <filesystem>
#include <span>
#include <string>
#include <string_view>

#include "core/map_file.h"

namespace mc
{
    enum class ProvinceKind : std::uint8_t
    {
        none = 0,
        land = 1,
        sea = 2,
        lake = 3,
        wasteland = 4,   // land no one owns or crosses (EU4's wasteland)
    };

    // The simulation's part of build/map/world.mcmap (docs/MAPFORMAT.md): the provinces, their kinds, regions,
    // neighbours, points and names, the regions and lands; no pixels, sites, blocks, borders, rivers or font (the
    // view's part, view/map_view.h). Arrays indexed by province id have `count` entries, id 0 meaning "no province".
    // All spans point into the sections it read.
    class MapData
    {
    public:
        // Reads and checks only this part's sections (mapcraft_sim; the game loads the whole map, MapView).
        bool load(const std::filesystem::path& path, std::string* error);

        std::uint32_t width = 0;     // map px (the header)
        std::uint32_t height = 0;
        std::uint32_t count = 0;

        std::span<const ProvinceKind> kind;
        std::span<const std::uint16_t> region;
        std::span<const std::uint8_t> region_parent;    // per region: its land (the region's parent), see land_name
        std::span<const std::uint32_t> adjacency_offsets;
        std::span<const std::uint16_t> adjacency_ids;
        std::span<const float> positions;   // count * 8 (PPOS): city, unit, text, port (x, y); NaN = none
        // The armies' march paths (tools/mapbake/marches.py), one per adjacency entry k (as adjacency_ids): the curve
        // from the province to that neighbour over the land of the two, from unit point to unit point, and its length
        // in map px; a neighbour that is not a land step has none (no points, length 0).
        std::span<const std::uint32_t> march_offsets;   // (MPTO) adjacency_ids.size() + 1: the points of entry k are
                                                        // [march_offsets[k], march_offsets[k + 1])
        std::span<const float> march_points;            // (MPTV) x, y pairs, map px
        std::span<const float> march_length;            // (MPTL) per entry, px
        std::span<const std::uint8_t> march_river;      // (MPTR) per entry, 1: the path crosses a river (the battle's
                                                        // river crossing, game/combat.h); 0 where it is not a step

        // Map px points of a province (positions): its city, where its armies stand (their march goes from one
        // province's to the next's, game/movement.h), and its name's anchor.
        struct Point
        {
            float x = 0.0f;
            float y = 0.0f;
        };
        [[nodiscard]] Point city_position(std::uint32_t id) const { return position(id, 0); }
        [[nodiscard]] Point unit_position(std::uint32_t id) const { return position(id, 1); }
        [[nodiscard]] Point text_position(std::uint32_t id) const { return position(id, 2); }
        // The adjacency entry of province `from`'s neighbour `to`, or -1 if they are not neighbours.
        [[nodiscard]] int adjacency_entry(std::uint32_t from, std::uint32_t to) const;
        // The march path of an adjacency entry: x, y pairs (empty if it is not a land step).
        [[nodiscard]] std::span<const float> march_path(int entry) const;

        [[nodiscard]] std::string_view province_name(std::uint32_t id) const;
        [[nodiscard]] std::uint32_t region_count() const { return region_count_; }
        [[nodiscard]] std::string_view region_name(std::uint32_t index) const;
        [[nodiscard]] std::uint32_t land_count() const { return land_count_; }
        [[nodiscard]] std::string_view land_name(std::uint32_t index) const;

    protected:
        // Reads and checks this part's sections from an open reader (MapView reads its own after them).
        void read(MapReader& reader);

        MapSections bytes_;

    private:
        [[nodiscard]] Point position(std::uint32_t id, std::size_t slot) const
        {
            const std::size_t i = std::size_t{ id } * 8 + slot * 2;
            return i + 1 < positions.size() ? Point{ positions[i], positions[i + 1] } : Point{};
        }

        MapStrings names_;
        MapStrings region_names_;
        MapStrings land_names_;
        std::uint32_t region_count_ = 0;
        std::uint32_t land_count_ = 0;
    };
}
