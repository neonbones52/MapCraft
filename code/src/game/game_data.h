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

    // Column lists. Every table of WorldState has one, next to it: one line per column, X(table, type, name, start),
    // in the saved order (game/saved_state.h). From it expand the table's fields (MC_DECLARE_COLUMN), its resize and
    // for_each_saved (the dump and the checksum of mapcraft_sim, later the save); a new column is one line. `table` is
    // the table's member in WorldState, the first half of the column's saved name ("provinces.owner"). `type` has no
    // commas. `start` says where the column's value at the start comes from (read by GameData, game_data.cpp):
    //   runtime                    0; from then on only the game changes it
    //   setup("csv column", parse) data/setup: read at load and written back by the editor (GameData::save_setup);
    //                              parse is country (a tag, empty = none; u16), key(&Content::list) (a key of that
    //                              list, empty = none; u8) or number (a number >= 0, empty = 0; float)
    //   by_hand                    data/setup, read and written by GameData's own code (a capital: a land province)
    //   derived                    not in data/setup: GameData sets it at load from other columns (a regiment's
    //                              morale: full)
    // What is not a column of a list is written out by hand where the lists expand: the sizes, the cores bit matrix,
    // the slot masks.
    // A line's own comment is /* */: a // comment would swallow the line continuation.
#define MC_DECLARE_COLUMN(table, type, name, start) Column<type> name;

#define MC_PROVINCE_COLUMNS(X)                                                                                  \
    X(provinces, std::uint16_t, owner, setup("owner", country))                                                 \
    X(provinces, std::uint16_t, controller, setup("controller", country))                                       \
    X(provinces, std::uint8_t, terrain, setup("terrain", key(&Content::terrain)))                               \
    /* the trade good the province produces: every land province has one (GameData::check) */                  \
    X(provinces, std::uint8_t, good, setup("good", key(&Content::goods)))                                       \
    X(provinces, float, base_tax, setup("base_tax", number))                                                    \
    X(provinces, float, population, setup("population", number))

    // Saved. Province state, one column per field, indexed by province id (MapData ids, 0 = none), padded(count)
    // long; strings never live here. data/setup/provinces.csv at the start: id and name (for people) by hand, then
    // the list's setup columns and the cores.
    struct Provinces
    {
        std::uint32_t count = 0;            // MapData::count: ids 0..count-1
        // Cores (by hand): a bit matrix, core_words u64 per province (64 countries a word): bit c of province p.
        std::uint32_t core_words = 1;
        MC_PROVINCE_COLUMNS(MC_DECLARE_COLUMN)
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

#define MC_REGION_COLUMNS(X)                                                                                    \
    X(regions, std::uint8_t, trade_node, setup("trade_node", key(&Content::trade_nodes)))

    // Saved. Region state (data/setup/regions.csv at the start: region, the map's number, and name for people by
    // hand), indexed by the map's region (1-based, 0 = none), padded(count) long.
    struct Regions
    {
        std::uint32_t count = 0;          // MapData::region_count(): regions 0..count-1
        MC_REGION_COLUMNS(MC_DECLARE_COLUMN)

        void resize(std::uint32_t region_count);
    };

#define MC_BALANCE_COLUMNS(X)                                                                                   \
    X(money, Money, cash, runtime)

    // Saved. Every balance of the world (docs/ECONOMY.md, "Деньги в i64"), one column of accounts; runtime only (no
    // setup file). Written only by the ledger (game/economy/ledger.h). No accounts yet: the countries' treasuries get
    // theirs with the treasury (M8), and with them the ledger's counters per kind of flow.
    struct Balances
    {
        MC_BALANCE_COLUMNS(MC_DECLARE_COLUMN)
    };

#define MC_COUNTRY_COLUMNS(X)                                                                                   \
    /* province id, 0 = none */                                                                                 \
    X(countries, std::uint16_t, capital, by_hand)                                                               \
    /* 1 = 100% (EU3's discipline: the damage its regiments deal and take, M6) */                               \
    X(countries, float, discipline, setup("discipline", number))                                                \
    /* the base maximum morale of its regiments */                                                              \
    X(countries, float, morale, setup("morale", number))

    // Saved. Country state (data/setup/countries.csv at the start: tag by hand), one column per field, indexed like
    // CountryDefinitions, padded(count) long.
    struct CountryStates
    {
        std::size_t count = 0;                  // countries, 0 = none included
        MC_COUNTRY_COLUMNS(MC_DECLARE_COLUMN)

        void resize(std::size_t country_count);
    };

    // Entities that are born and die (armies, regiments) live in slots: a fixed number per table, so the tick never
    // allocates; a mask of the slots in use; a new entity takes the lowest free slot (game/armies.h). A freed slot's
    // columns are back to 0. The numbers are the limits: regiments fill the u16 slot index (more needs u32 slots),
    // which is far above what EU4 fields on a map six times ours.
    inline constexpr std::uint32_t kArmySlots = 4096;
    inline constexpr std::uint32_t kRegimentSlots = 65536;
    static_assert(kArmySlots % 64 == 0 && kRegimentSlots % 64 == 0, "whole words of the slot masks");
    static_assert(kArmySlots <= 65536 && kRegimentSlots <= 65536, "u16 slots");

    // The used part of a slot table: the slots up to the end of the last mask word with a live slot (a multiple of 64).
    [[nodiscard]] inline std::size_t used_slots(const Column<std::uint64_t>& alive)
    {
        std::size_t words = alive.size();
        while (words > 0 && alive[words - 1] == 0) {
            --words;
        }
        return words * 64;
    }

#define MC_ARMY_COLUMNS(X)                                                                                      \
    X(armies, std::uint16_t, owner, setup("owner", country))                                                    \
    /* a land province id: where it is (a marching army stays in it until it arrives in the next) */            \
    X(armies, std::uint16_t, province, by_hand)                                                                 \
    /* the march (game/movement.h): the neighbour it is marching into, 0 = standing */                          \
    X(armies, std::uint16_t, next, runtime)                                                                     \
    /* where the march ends, 0 = standing */                                                                    \
    X(armies, std::uint16_t, destination, runtime)                                                              \
    /* the step done so far, in the units of the step's cost (Movement::step_cost) */                           \
    X(armies, float, progress, runtime)                                                                         \
    /* grows each time the slot is freed: what a reference from outside checks (ArmyRef, game/armies.h) */     \
    X(armies, std::uint16_t, generation, runtime)

    // Saved. The armies (land only; fleets are not here), in slots, kArmySlots long. data/setup/armies.csv at the
    // start: army (its number in the file, for regiments.csv) and province by hand; an army's name is not data.
    struct Armies
    {
        MC_ARMY_COLUMNS(MC_DECLARE_COLUMN)
        Column<std::uint64_t> alive;   // by hand: bit s % 64 of word s / 64 = slot s holds an army

        void resize();
    };

#define MC_REGIMENT_COLUMNS(X)                                                                                  \
    /* its army's slot: a reference within one life, the army frees its regiments when it dies */               \
    X(regiments, std::uint16_t, army, by_hand)                                                                  \
    X(regiments, std::uint8_t, type, setup("type", key(&Content::regiment_types)))                              \
    /* men; a full regiment is 1000 */                                                                          \
    X(regiments, float, strength, setup("strength", number))                                                    \
    /* full at the start: the morale of its army's owner */                                                     \
    X(regiments, float, morale, derived)

    // Saved. The regiments of every army, in slots, kRegimentSlots long. data/setup/regiments.csv at the start: army
    // (its number in armies.csv) by hand.
    struct Regiments
    {
        MC_REGIMENT_COLUMNS(MC_DECLARE_COLUMN)
        Column<std::uint64_t> alive;   // by hand, like Armies::alive

        void resize();
    };

    // The regiment types' categories (regiment_types.csv `category`): what a type is in battle (M6).
    enum class RegimentCategory : std::uint8_t
    {
        infantry,
        cavalry,
        artillery,
    };
    inline constexpr std::string_view kRegimentCategories[] = { "infantry", "cavalry", "artillery" };

    // The money good: worth 1 emerald whatever goods.csv says (docs/GAMEDATA.md, "Файлы").
    inline constexpr std::string_view kMoneyGood = "emerald";

    // Content: what the game is made of (data/common and the map): lists and definitions, no state. Fixed once
    // loaded; strings live here and only here (UI, editor, logs), never in WorldState.
    struct Content
    {
        DefinitionList terrain;
        std::vector<float> terrain_movement_cost;   // terrain.csv `movement_cost`: a march's multiplier over it (the
                                                    // plains 1; game/movement.h); [0] (no terrain) 1
        DefinitionList goods;
        std::vector<float> goods_price;   // goods.csv `price`: the base price, emeralds per unit produced (the money
                                          // good: 1); no rule reads it yet
        DefinitionList trade_nodes;       // data/common/trade_nodes.csv: a node is made of whole regions (M11 rules)
        DefinitionList regiment_types;    // data/common/regiment_types.csv: the units (EU4's Latin Knights...)
        std::vector<RegimentCategory> regiment_category;   // per type; its combat numbers come with M6
        CountryDefinitions countries;
        std::vector<std::uint16_t> province_region;   // from the map (MapData::region), for the rules
    };

    // Saved: the state of the world, SoA columns only (indices into Content, numbers); what the simulation changes.
    // Its start comes from data/setup and the defaults (the lists' `start`). Every column is in game/saved_state.h.
    struct WorldState
    {
        Provinces provinces;
        Regions regions;
        Balances money;
        CountryStates countries;
        Armies armies;
        Regiments regiments;
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
        // Rewrites data/setup/provinces.csv (every land province, by id), regions.csv (every region, by number),
        // countries.csv, armies.csv and regiments.csv (the live ones in slot order, armies numbered 1, 2, ... in it)
        // from `state`, each in its `setup_layout` order.
        bool save_setup(const std::filesystem::path& dir, const MapData& map, std::string* error) const;
        // The checks that are warnings at load, for the current state: a capital its country does not own; a land
        // province without a good; an army without an owner or regiments, a regiment without a type; once there are
        // trade nodes, a region without one and a node without regions.
        [[nodiscard]] std::vector<std::string> check(const MapData& map) const;
        // "11 countries, 5 terrain, ..." for the logs.
        [[nodiscard]] std::string summary() const;

        bool loaded = false;
        Content content;
        WorldState state;
        std::vector<std::string> warnings;
        // The columns of each data/setup file in the order they were loaded (the game's own only; an absent file: the
        // default order), which save_setup keeps. Neither state nor content: how the files are laid out.
        struct SetupLayout
        {
            std::vector<std::string> provinces;
            std::vector<std::string> regions;
            std::vector<std::string> countries;
            std::vector<std::string> armies;
            std::vector<std::string> regiments;
        } setup_layout;
    };
}
