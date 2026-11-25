#pragma once


#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <new>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "core/csv.h"
#include "core/money.h"

namespace mc
{
    class MapData;

    // Columns are padded to whole SIMD batches and 64-byte aligned (docs/GAMEDATA.md).
    //
    // Every piece of the game's data carries one of three labels (docs/GAMEDATA.md, "Метки колонок"):
    //   saved    the state: what the days change and a save holds (WorldState and its parts, listed column by column
    //            in game/saved_state.h; the dump and the checksum of mapcraft_sim walk that list)
    //   content  what the game is made of, fixed once loaded from data/common and the map (Content and its parts)
    //   cache    rebuilt from the other two after a load, deterministically, never saved (none yet: the day's
    //            working columns of the rules to come)
    // Sizes (counts, strides) are neither: they follow from the content and the map.
    inline constexpr std::size_t kLanes = 16;
    inline constexpr std::size_t padded(std::size_t n) { return (n + kLanes - 1) / kLanes * kLanes; }

    template <class T>
    struct AlignedAllocator
    {
        using value_type = T;
        static constexpr std::align_val_t kAlign{ 64 };
        AlignedAllocator() = default;
        template <class U>
        AlignedAllocator(const AlignedAllocator<U>&) noexcept {}
        T* allocate(std::size_t n) { return static_cast<T*>(::operator new(n * sizeof(T), kAlign)); }
        void deallocate(T* p, std::size_t) noexcept { ::operator delete(p, kAlign); }
        template <class U>
        bool operator==(const AlignedAllocator<U>&) const noexcept { return true; }
    };
    template <class T>
    using Column = std::vector<T, AlignedAllocator<T>>;

    // Content. One list of data/common: terrain, goods, ... Index 0 is "none" (empty key); the rest in file order.
    struct DefinitionList
    {
        std::vector<std::string> key;
        std::vector<std::string> name;
        std::vector<std::uint32_t> color;   // RGBA8, r in the low byte (like MapData::color)

        [[nodiscard]] std::size_t size() const { return key.size(); }
        // Index of a key, 0 for an empty key, -1 if unknown.
        [[nodiscard]] int find(std::string_view k) const;
    };

    // Content. Country definitions (data/common/countries.csv): fixed once loaded. Index 0 = none; the same index
    // as CountryStates.
    struct CountryDefinitions
    {
        std::vector<std::uint32_t> tag;     // three ASCII letters packed little-endian
        std::vector<std::string> name;
        std::vector<std::string> adjective; // the names of parts cut off from the capital (EU4's enclaves)
        std::vector<std::uint32_t> color;

        [[nodiscard]] std::size_t size() const { return tag.size(); }
        [[nodiscard]] int find(std::string_view tag_text) const;   // 0 for empty, -1 if unknown
        [[nodiscard]] std::string tag_text(std::size_t country) const;
    };

    // Saved. Province state, one column per field, indexed by province id (MapData ids, 0 = none), padded(count)
    // long. Hot (changes in the simulation) and cold fields are separate columns anyway; strings never live here.
    struct Provinces
    {
        std::uint32_t count = 0;            // MapData::count: ids 0..count-1
        // hot
        Column<std::uint16_t> owner;
        Column<std::uint16_t> controller;
        // cold
        Column<std::uint8_t> terrain;
        Column<std::uint8_t> rgo;        // Content::rgo_types index, 0 = none (becomes the province's good, M5 step 2)
        // Cores: a bit matrix, core_words u64 per province (64 countries a word): bit c of province p.
        std::uint32_t core_words = 1;
        Column<std::uint64_t> cores;

        void resize(std::uint32_t province_count, std::size_t country_count);
        [[nodiscard]] bool has_core(std::uint32_t province, std::size_t country) const
        {
            return (cores[std::size_t{ province } * core_words + country / 64] >> (country % 64)) & 1u;
        }
        void set_core(std::uint32_t province, std::size_t country, bool on)
        {
            std::uint64_t& word = cores[std::size_t{ province } * core_words + country / 64];
            const std::uint64_t bit = std::uint64_t{ 1 } << (country % 64);
            word = on ? (word | bit) : (word & ~bit);
        }
    };

    // Saved. Region state (data/setup/regions.csv at the start), indexed by the map's region (1-based, 0 = none),
    // padded(count) long.
    struct Regions
    {
        std::uint32_t count = 0;          // MapData::region_count(): regions 0..count-1
        Column<std::uint8_t> trade_node;  // Content::trade_nodes index, 0 = none

        void resize(std::uint32_t region_count);
    };

    // Saved. Every balance of the world (docs/ECONOMY.md, "Деньги в i64"), one column of accounts. Written only by the
    // ledger (game/economy/ledger.h). No accounts yet: the countries' treasuries get theirs with the treasury (M8),
    // and with them the ledger's counters per kind of flow.
    struct Balances
    {
        Column<Money> cash;
    };

    // Saved. Country state (data/setup/countries.csv at the start), one column per field, indexed like
    // CountryDefinitions, padded(count) long.
    struct CountryStates
    {
        std::size_t count = 0;                  // countries, 0 = none included
        Column<std::uint16_t> capital;          // province id, 0 = none

        void resize(std::size_t country_count);
    };

    // The money good: worth 1 emerald whatever goods.csv says (docs/GAMEDATA.md, "Файлы").
    inline constexpr std::string_view kMoneyGood = "emerald";

    // Content: what the game is made of (data/common and the map): lists and definitions, no state. Fixed once
    // loaded; strings live here and only here (UI, editor, logs), never in WorldState.
    struct Content
    {
        DefinitionList terrain;
        DefinitionList goods;
        std::vector<float> goods_price;   // goods.csv `price`, emeralds per unit (the money good: 1); no rule reads it yet
        DefinitionList rgo_types;         // data/common/rgo_types.csv: the keys of `rgo` until it becomes the good
        DefinitionList trade_nodes;       // data/common/trade_nodes.csv: a node is made of whole regions (M11 rules)
        CountryDefinitions countries;
        std::vector<std::uint16_t> province_region;   // from the map (MapData::region), for the rules
    };

    // Saved: the state of the world, SoA columns only (indices into Content, numbers); what the simulation changes.
    // Loaded from data/setup (the start). Every column is listed in game/saved_state.h.
    struct WorldState
    {
        Provinces provinces;
        Regions regions;
        Balances money;
        CountryStates countries;
    };

    // Loads Content and the start's WorldState from data/ (docs/GAMEDATA.md) and writes the start back (editor). In
    // the game a World owns one (game/world.h) and hands out the content read-only; the editor owns one and edits
    // only its state.
    class GameData
    {
    public:
        // Errors (unknown keys, duplicates, bad numbers, a setup row for a province that is not land) fail the load
        // with file:line; inconsistencies an editing session may pass through (a capital its country does not own)
        // go to `warnings`. `patches` (a scenario's, mapcraft_sim) change fields of the files before they are parsed;
        // each must hit a file the load reads.
        bool load(const std::filesystem::path& dir, const MapData& map, std::string* error,
                  std::span<const csv::Patch> patches = {});
        // Rewrites data/setup/provinces.csv (every land province, by id), regions.csv (every region, by number) and
        // countries.csv from `state`.
        bool save_setup(const std::filesystem::path& dir, const MapData& map, std::string* error) const;
        // The checks that are warnings at load, for the current state: a capital its country does not own; once there
        // are trade nodes, a region without one and a node without regions.
        [[nodiscard]] std::vector<std::string> check(const MapData& map) const;
        // "11 countries, 5 terrain, ..." for the logs.
        [[nodiscard]] std::string summary() const;

        bool loaded = false;
        Content content;
        WorldState state;
        std::vector<std::string> warnings;
    };
}
