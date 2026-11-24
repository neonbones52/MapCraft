#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <new>
#include <string>
#include <string_view>
#include <vector>

#include "core/money.h"

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

    // One list of data/common: terrain, goods, ... Index 0 is "none" (empty key); the rest in file order.
    struct DefinitionList
    {
        std::vector<std::string> key;
        std::vector<std::string> name;
        std::vector<std::uint32_t> color;   // RGBA8, r in the low byte (like MapData::color)

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
        // cold
        Column<std::uint8_t> terrain;
        Column<std::uint8_t> rgo;        // RgoTypes index, 0 = none
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

    // Pops (data/setup/pops.csv at the start): one per province and pop type, a matrix of rows by type, each row a
    // province column: type t of province p at t * stride + p, stride = padded(province count), so each row is
    // 64-byte aligned. A pop's place (t, p) is its id, stable; size 0 = no pop. Row 0 ("none") stays empty.
    struct Pops
    {
        std::uint32_t types = 0;        // PopTypes size, "none" included
        std::size_t stride = 0;
        Column<float> size;             // people

        void resize(std::uint32_t province_count, std::uint32_t type_count);
        [[nodiscard]] float at(std::size_t type, std::uint32_t province) const { return size[type * stride + province]; }
        float& at(std::size_t type, std::uint32_t province) { return size[type * stride + province]; }
    };

    // What money moved for: the ledger's counters, one per rule that moves money (docs/ECONOMY.md).
    enum class Flow : std::uint8_t
    {
        mine,    // emeralds minted by an RGO
        count,
    };
    inline constexpr std::size_t kFlows = static_cast<std::size_t>(Flow::count);

    // Every balance of the world (docs/ECONOMY.md, "Деньги в i64"), one column of accounts: the pops' cash, the
    // account of a pop being its place in Pops (t * stride + p); treasuries, buildings and the pool get ranges of
    // their own when they come. Written only by the ledger (game/economy/ledger.h).
    struct Balances
    {
        Column<Money> cash;
        Money start = 0;                       // the sum of all cash at the start
        Money minted = 0;                      // all money minted since
        std::array<Money, kFlows> flows{};     // money moved per flow since the start
    };

    // Country state (data/setup/countries.csv at the start), one column per field, indexed like CountryDefinitions,
    // padded(count) long.
    struct CountryStates
    {
        std::size_t count = 0;                  // countries, 0 = none included
        Column<std::uint16_t> capital;          // province id, 0 = none

        void resize(std::size_t country_count);
    };

    // The tiers of a pop's needs, in the order they are met.
    inline constexpr std::size_t kNeedTiers = 3;
    inline constexpr std::array<const char*, kNeedTiers> kNeedTierKeys = { "essential", "everyday", "luxury" };
    inline constexpr std::array<const char*, kNeedTiers> kNeedTierNames = { "Essential", "Everyday", "Luxury" };
    // The money good: the only one no need consumes (docs/ECONOMY.md).
    inline constexpr std::string_view kMoneyGood = "emerald";

    // Pop types (data/common/pop_types.csv): classes of behaviour; what they need, per person a day and tier.
    struct PopTypes
    {
        DefinitionList list;                                // index 0 = none
        std::vector<std::array<float, kNeedTiers>> need;
        std::vector<float> start_cash;                      // emeralds per person at the start

        [[nodiscard]] std::size_t size() const { return list.size(); }
    };

    // The goods that fill each tier of needs (data/common/needs.csv), interchangeable by weight: 1 meat at 1.5 fills
    // as much of the tier as 1.5 wheat at 1.
    struct NeedGoods
    {
        std::array<std::vector<std::uint8_t>, kNeedTiers> good;   // Content::goods index
        std::array<std::vector<float>, kNeedTiers> weight;
    };

    // RGO types (data/common/rgo_types.csv): who works there and what one worker makes a day.
    struct RgoTypes
    {
        DefinitionList list;                         // index 0 = none
        std::vector<std::uint8_t> workers;           // PopTypes index
        std::vector<std::uint8_t> owners;            // PopTypes index: the pop of the province its earnings go to
        std::vector<std::uint32_t> output_first;     // type t makes outputs [output_first[t], output_first[t + 1])
        std::vector<std::uint8_t> output_good;       // Content::goods index
        std::vector<float> output_amount;

        [[nodiscard]] std::size_t size() const { return list.size(); }
    };

    // What the game is made of (data/common): lists and definitions, no state. Fixed once loaded; strings live here
    // and only here (UI, editor, logs), never in WorldState.
    struct Content
    {
        DefinitionList terrain;
        DefinitionList goods;
        PopTypes pop_types;
        NeedGoods needs;
        RgoTypes rgo_types;
        CountryDefinitions countries;
    };

    // The state of the world: SoA columns only (indices into Content, numbers); what the simulation changes. Loaded
    // from data/setup (the start).
    struct WorldState
    {
        Provinces provinces;
        Pops pops;
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
        // go to `warnings`.
        bool load(const std::filesystem::path& dir, const MapData& map, std::string* error);
        // Rewrites data/setup/provinces.csv (every land province, by id), pops.csv (every pop of size > 0, by
        // province and type) and countries.csv from `state`.
        bool save_setup(const std::filesystem::path& dir, const MapData& map, std::string* error) const;
        // The checks that are warnings at load, for the current state: a capital its country does not own, an orphan
        // good (nothing makes it, or no need takes it; money excepted), a tier some pop type needs but no good fills.
        [[nodiscard]] std::vector<std::string> check(const MapData& map) const;
        // "11 countries, 5 terrain, ..." for the logs.
        [[nodiscard]] std::string summary() const;

        bool loaded = false;
        Content content;
        WorldState state;
        std::vector<std::string> warnings;
    };
}
