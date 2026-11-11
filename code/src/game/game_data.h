#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <new>
#include <string>
#include <string_view>
#include <vector>

namespace mc
{
    class MapData;

    // Columns are padded to whole SIMD batches and 64-byte aligned (docs/GAMEDATA.md).
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

    // One list of data/common: terrain, cultures, religions, ... Index 0 is "none" (empty key); the rest in file order.
    struct DefinitionList
    {
        std::vector<std::string> key;
        std::vector<std::string> name;
        std::vector<std::uint32_t> color;   // RGBA8, r in the low byte (like MapData::color)
        std::vector<std::uint8_t> group;    // cultures and religions: index into their group list, 0 = none

        [[nodiscard]] std::size_t size() const { return key.size(); }
        // Index of a key, 0 for an empty key, -1 if unknown.
        [[nodiscard]] int find(std::string_view k) const;
    };

    // Country definitions (data/common/countries.csv): content, fixed once loaded. Index 0 = none; the same index
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

    // Province state, one column per field, indexed by province id (MapData ids, 0 = none), padded(count) long.
    // Hot (changes in the simulation) and cold fields are separate columns anyway; strings never live here.
    struct Provinces
    {
        std::uint32_t count = 0;            // MapData::count: ids 0..count-1
        // hot
        Column<std::uint16_t> owner;
        Column<std::uint16_t> controller;
        Column<float> tax;
        Column<float> production;
        Column<float> manpower;
        Column<std::uint8_t> culture;
        Column<std::uint8_t> religion;
        // cold
        Column<std::uint8_t> terrain;
        Column<std::uint8_t> trade_good;
        Column<std::uint8_t> trade_node;
        Column<std::uint8_t> center_of_trade;   // 0 / 1
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

    // Country state (data/setup/countries.csv at the start), one column per field, indexed like CountryDefinitions,
    // padded(count) long.
    struct CountryStates
    {
        std::size_t count = 0;                  // countries, 0 = none included
        Column<std::uint16_t> capital;          // province id, 0 = none
        Column<std::uint8_t> primary_culture;   // Content::cultures index, 0 = none
        Column<std::uint8_t> religion;          // state religion: Content::religions index, 0 = none

        void resize(std::size_t country_count);
    };

    // What the game is made of (data/common): lists and definitions, no state. Fixed once loaded; strings live here
    // and only here (UI, editor, logs), never in WorldState.
    struct Content
    {
        DefinitionList terrain;
        DefinitionList culture_groups;
        DefinitionList cultures;
        DefinitionList religion_groups;
        DefinitionList religions;
        DefinitionList trade_goods;   // data/common/trade_goods.csv, optional until trade is made
        DefinitionList trade_nodes;   // data/common/trade_nodes.csv, optional until trade is made
        CountryDefinitions countries;
    };

    // The state of the world: SoA columns only (indices into Content, numbers); what the simulation changes. Loaded
    // from data/setup (the start).
    struct WorldState
    {
        Provinces provinces;
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
        // go to `warnings`.
        bool load(const std::filesystem::path& dir, const MapData& map, std::string* error);
        // Rewrites data/setup/provinces.csv (every land province, by id) and data/setup/countries.csv from `state`.
        bool save_setup(const std::filesystem::path& dir, const MapData& map, std::string* error) const;
        // The checks that are warnings at load, for the current state.
        [[nodiscard]] std::vector<std::string> check(const MapData& map) const;
        // "11 countries, 5 terrain, ..." for the logs.
        [[nodiscard]] std::string summary() const;

        bool loaded = false;
        Content content;
        WorldState state;
        std::vector<std::string> warnings;
    };
}
