#pragma once


#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <new>
#include <span>
#include <string>
#include <string_view>
#include <type_traits>
#include <vector>

#include "core/csv.h"
#include "core/fixed.h"
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
    //   cache    rebuilt from the other two after a load, a restart and an edit, deterministically, never saved
    //            (Cache: the countries' modifier sums; the rules' own, like the march's graph, live with the rules)
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
    //                              list, empty = none; u8), number (a number >= 0, empty = 0; float) or whole (a
    //                              whole number >= 0, empty = 0; i32)
    //   by_hand                    data/setup, read and written by GameData's own code (a capital: a land province)
    //   derived                    not in data/setup: GameData sets it at load from other columns (a regiment's
    //                              morale: full)
    // What is not a column of a list is written out by hand where the lists expand: the sizes, the bit matrices of
    // the cores and of the countries' modifier bundles, the slot masks.
    // A line's own comment is /* */: a // comment would swallow the line continuation.
#define MC_DECLARE_COLUMN(table, type, name, start) Column<type> name;

#define MC_PROVINCE_COLUMNS(X)                                                                                  \
    X(provinces, std::uint16_t, owner, setup("owner", country))                                                 \
    X(provinces, std::uint16_t, controller, setup("controller", country))                                       \
    X(provinces, std::uint8_t, terrain, setup("terrain", key(&Content::terrain)))                               \
    /* the trade good the province produces: every land province has one (GameData::check) */                  \
    X(provinces, std::uint8_t, good, setup("good", key(&Content::goods)))                                       \
    X(provinces, float, base_tax, setup("base_tax", number))                                                    \
    X(provinces, float, population, setup("population", number))                                                \
    /* the fort's level (EU4's: castle 2, bastion 4, star fort 6, fortress 8; 0 none); a capital has */         \
    /* capital_fort_level more (game/occupation.h) */                                                           \
    X(provinces, std::uint8_t, fort, setup("fort", whole))                                                      \
    /* the garrison's men: full at the start (game/occupation.h), 0 without a fort */                           \
    X(provinces, std::int32_t, garrison, derived)                                                               \
    /* the siege's progress (EU4's siege status): kept while the province is besieged, then down 1 a day; */    \
    /* 0 when its controller changes */                                                                         \
    X(provinces, std::uint8_t, siege_progress, runtime)

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

    // The accounts of a country (docs/GAMEDATA.md, "Деньги"): the four the budget sliders put the monthly income into
    // (EU3's investments: the areas of technology and stability; what they buy is M9's) and the treasury (the
    // Treasury slider's share, the census, later the upkeep and the costs). In the sliders' order.
    enum class Account : std::uint8_t
    {
        army = 0,
        production = 1,
        government = 2,
        stability = 3,
        treasury = 4,
    };
    inline constexpr std::size_t kAccountsPerCountry = 5;
    inline constexpr std::string_view kAccountNames[kAccountsPerCountry] = { "army", "production", "government",
                                                                             "stability", "treasury" };
    // The account's index in Balances::cash: kAccountsPerCountry a country, in Account's order (country 0 has its
    // own, never used).
    [[nodiscard]] constexpr std::size_t account(std::size_t country, Account kind)
    {
        return country * kAccountsPerCountry + static_cast<std::size_t>(kind);
    }

    // Saved. Every balance of the world (docs/ECONOMY.md, "Деньги в i64"), one column of accounts (account() above),
    // padded(countries × kAccountsPerCountry) long; runtime only (no setup file). Written only by the ledger
    // (game/economy/ledger.h).
    struct Balances
    {
        MC_BALANCE_COLUMNS(MC_DECLARE_COLUMN)

        void resize(std::size_t country_count);
    };

#define MC_COUNTRY_COLUMNS(X)                                                                                   \
    /* province id, 0 = none */                                                                                 \
    X(countries, std::uint16_t, capital, by_hand)                                                               \
    /* the budget sliders (game/economy/budget.h): the shares of the monthly income, whole percents adding */   \
    /* up to 100, in Account's order; at the start defines.csv's budget_* (the same for every country) */       \
    X(countries, std::uint8_t, budget_army, derived)                                                            \
    X(countries, std::uint8_t, budget_production, derived)                                                      \
    X(countries, std::uint8_t, budget_government, derived)                                                      \
    X(countries, std::uint8_t, budget_stability, derived)                                                       \
    X(countries, std::uint8_t, budget_treasury, derived)                                                        \
    /* EU3's inflation, in percent (4 = everything 4% dearer), >= 0: grows with the Treasury share */           \
    X(countries, float, inflation, runtime)                                                                     \
    /* the land maintenance slider, whole percent maintenance_min..100 (game/economy/upkeep.h); at the start */ \
    /* defines.csv's maintenance_start */                                                                       \
    X(countries, std::uint8_t, maintenance, derived)

    // Saved. Country state (data/setup/countries.csv at the start: tag by hand), one column per field, indexed like
    // CountryDefinitions, padded(count) long.
    struct CountryStates
    {
        std::size_t count = 0;                  // countries, 0 = none included
        // The modifier bundles it has (by hand; the sources of its modifiers, game/modifiers.h): a bit matrix,
        // modifier_words u64 per country (64 bundles a word): bit b of country c. The base bundle is not in it: every
        // country has that one.
        std::uint32_t modifier_words = 1;
        MC_COUNTRY_COLUMNS(MC_DECLARE_COLUMN)
        Column<std::uint64_t> modifiers;

        void resize(std::size_t country_count, std::size_t bundle_count);
        [[nodiscard]] bool has_bundle(std::size_t country, std::size_t bundle) const
        {
            return (modifiers[country * modifier_words + bundle / 64] >> (bundle % 64)) & 1u;
        }
        void set_bundle(std::size_t country, std::size_t bundle, bool on)
        {
            std::uint64_t& word = modifiers[country * modifier_words + bundle / 64];
            const std::uint64_t bit = std::uint64_t{ 1 } << (bundle % 64);
            word = on ? (word | bit) : (word & ~bit);
        }
    };

    // The budget sliders' columns, in Account's order: kBudgetShares[a] is the share that goes to account a.
    inline constexpr Column<std::uint8_t> CountryStates::* kBudgetShares[kAccountsPerCountry] = {
        &CountryStates::budget_army, &CountryStates::budget_production, &CountryStates::budget_government,
        &CountryStates::budget_stability, &CountryStates::budget_treasury
    };
    // One country's sliders, in Account's order.
    using BudgetShares = std::array<std::uint8_t, kAccountsPerCountry>;

    // The modifiers (docs/GAMEDATA.md, "Модификаторы"): what a source gives a country. A source is a bundle of
    // data/common/modifiers.csv (bundle, modifier, value); a country's sum of a modifier is the sum of the values of
    // the base bundle and of every bundle it has (EU4's way: values add up, whatever they mean). Values and sums are
    // Fixed (core/fixed.h), as EU4's CFixedPoint: the additions are exact. What a sum means is its formula's
    // (game/modifiers.h): a flat value, or a share where 0.1 = +10%. The set of modifiers is the code's: one line
    // each, X(name), the name being the modifier's key in the data; a new modifier is one line here and the formula
    // that reads it. The base bundle holds EU4's values at military technology 3 (docs/GAMEDATA.md).
#define MC_COUNTRY_MODIFIERS(X)                                                                                 \
    /* flat, 1 = 100%: EU3's discipline, the damage its regiments deal and take (the base gives 1) */           \
    X(discipline)                                                                                               \
    /* flat: the base of its regiments' maximum morale, before land_morale (the base bundle; technology, M9) */ \
    X(base_morale)                                                                                              \
    /* a share: the maximum morale is base_morale × (1 + land_morale), EU4's "morale of armies" */             \
    X(land_morale)                                                                                              \
    /* flat: EU4's military tactics; the tactics its regiments are shot at with are this × discipline */        \
    X(military_tactics)                                                                                         \
    /* flat, whole: the width of its front line in battle (EU4's combat width; technology widens it, M9) */      \
    X(combat_width)                                                                                             \
    /* flat: the weapons' damage of a category in a phase (EU4's technology values: infantry_fire...) */        \
    X(infantry_fire)                                                                                            \
    X(infantry_shock)                                                                                           \
    X(cavalry_fire)                                                                                             \
    X(cavalry_shock)                                                                                            \
    X(artillery_fire)                                                                                           \
    X(artillery_shock)                                                                                          \
    /* shares: EU4's siege ability (its sieges' phases shorter) and fort defence (the sieges of its forts' */   \
    /* phases longer) */                                                                                        \
    X(siege_ability)                                                                                            \
    X(fort_defence)                                                                                             \
    /* flat, whole: the most its artillery adds to a siege (EU4's artillery_levels_available_vs_fort, 5) */     \
    X(artillery_levels_available_vs_fort)                                                                       \
    /* flat, whole: the highest fort level it can build (EU4's _nHighestPossibleFort: castle 2 at the */        \
    /* start; technology raises it, M9): forts below it are obsolete to its sieges, its progress cap grows */   \
    X(highest_fort)

#define MC_MODIFIER_NAME(name) #name,
    inline constexpr std::string_view kCountryModifiers[] = { MC_COUNTRY_MODIFIERS(MC_MODIFIER_NAME) };
#undef MC_MODIFIER_NAME
    inline constexpr std::size_t kCountryModifierCount = std::size(kCountryModifiers);

    // Content. The modifier bundles of data/common/modifiers.csv: a list (index 0 = none, the rest in the order of
    // their first line), and one column per modifier over the bundles, the bundle's value (0 = it does not give it).
#define MC_DECLARE_BUNDLE_VALUES(name) std::vector<Fixed> name;
    struct ModifierBundles
    {
        DefinitionList list;
        std::uint8_t base = 0;   // the bundle every country has (key kBaseBundle), 0 = none in the file
        MC_COUNTRY_MODIFIERS(MC_DECLARE_BUNDLE_VALUES)
    };
#undef MC_DECLARE_BUNDLE_VALUES
    inline constexpr std::string_view kBaseBundle = "base";   // EU4's base_values

    // Cache. Every country's sum of each modifier, one column per modifier, indexed like CountryStates, padded(count)
    // long: rebuilt from the bundles and the countries' sets after a load, a restart and an edit of a set
    // (GameData::rebuild_cache); the formulas read them (game/modifiers.h).
#define MC_DECLARE_SUM(name) Column<Fixed> name;
    struct CountryModifiers
    {
        std::size_t count = 0;
        MC_COUNTRY_MODIFIERS(MC_DECLARE_SUM)
    };
#undef MC_DECLARE_SUM

    // Cache. Who is at war with whom (game/war.h), from the wars (Wars): a symmetric bit matrix, words u64 per
    // country (64 countries a word): bit b of country a's row = a and b are at war.
    struct AtWar
    {
        std::size_t count = 0;   // countries, 0 = none included
        std::uint32_t words = 1;
        Column<std::uint64_t> bits;

        [[nodiscard]] bool has(std::size_t a, std::size_t b) const
        {
            return a < count && b < count && ((bits[a * words + b / 64] >> (b % 64)) & 1u);
        }
    };

    // Cache: the rules' data rebuilt from Content and WorldState (deterministically: CLAUDE.md, rule 9), never saved.
    // for_each_cached (game/modifiers.h) walks it for mapcraft_sim's cache dump.
    struct Cache
    {
        CountryModifiers country_modifiers;
        AtWar at_war;
    };

    // Entities that are born and die (armies, regiments, leaders, battles, wars, sieges) live in slots: a fixed number
    // per table, so a birth allocates nothing and the order is deterministic (CLAUDE.md lets the tick allocate, but
    // not per entity of a daily pass); a mask of the slots in use; a new entity takes the lowest free slot
    // (game/armies.h). A freed slot's columns are back to 0. The numbers are the limits: regiments fill the u16 slot index but for the
    // last word (a battle names a regiment by its slot + 1 in a u16, 0 = none: Battle, Regiments::reserve_prev and
    // reserve_next; more needs u32 slots), which is far above what EU4 fields on a map six times ours; a battle is one
    // per province at most; EU4's countries keep a few leaders each.
    inline constexpr std::uint32_t kArmySlots = 4096;
    inline constexpr std::uint32_t kRegimentSlots = 65472;
    inline constexpr std::uint32_t kLeaderSlots = 1024;
    inline constexpr std::uint32_t kBattleSlots = 1024;
    inline constexpr std::uint32_t kWarSlots = 256;
    inline constexpr std::uint32_t kSiegeSlots = 1024;   // one a province at most, like the battles
    inline constexpr std::uint32_t kTruceSlots = 256;    // like the wars: a truce follows a war
    static_assert(kArmySlots % 64 == 0 && kRegimentSlots % 64 == 0 && kLeaderSlots % 64 == 0 &&
                      kBattleSlots % 64 == 0 && kWarSlots % 64 == 0 && kSiegeSlots % 64 == 0 &&
                      kTruceSlots % 64 == 0,
                  "whole words of the slot masks");
    static_assert(kLeaderSlots < 65536, "an army names its leader by slot + 1 in a u16");
    static_assert(kArmySlots <= 65536 && kRegimentSlots < 65536, "u16 slots, a regiment's slot + 1 too");

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
    X(armies, std::uint16_t, generation, runtime)                                                               \
    /* the battle it fights in (game/combat.h): its slot + 1, 0 = none */                                      \
    X(armies, std::uint16_t, battle, runtime)                                                                   \
    /* its side there (Battle::sides): 0 the attacker, 1 the defender; 0 out of a battle */                     \
    X(armies, std::uint8_t, side, runtime)                                                                      \
    /* the day of the battle it joined on (Battle::duration then): with the slot, EU4's order of a side's */      \
    /* armies, which picks the commander of equal ones */                                                        \
    X(armies, std::uint16_t, joined, runtime)                                                                   \
    /* its general (Leaders): slot + 1, 0 = none; at the start from data/setup/leaders.csv's army column */       \
    X(armies, std::uint16_t, leader, derived)

    // Saved. The armies (land only; fleets are not here), in slots, kArmySlots long. data/setup/armies.csv at the
    // start: army (its number in the file, for regiments.csv and leaders.csv) and province by hand; an army's name is
    // not data.
    struct Armies
    {
        MC_ARMY_COLUMNS(MC_DECLARE_COLUMN)
        Column<std::uint64_t> alive;        // by hand: bit s % 64 of word s / 64 = slot s holds an army
        Column<std::uint64_t> retreating;   // by hand, the same bits: the army retreats from a lost battle (its march
                                            // is locked, it starts no battle; game/combat.h)

        void resize();
    };

    // Where a regiment is in its army's battle (Regiments::place): 0 not in a battle, 1..kFrontSlots a position of the
    // front line + 1 (EU4's 0..MAX_COMBAT_WIDTH), kPlaceBack + a position of the back line (below), in the reserve, or
    // out of the lines (broken: still in its army, still counted, never back in this battle).
    inline constexpr std::uint8_t kPlaceNone = 0;
    inline constexpr std::uint8_t kPlaceReserve = 254;
    inline constexpr std::uint8_t kPlaceOut = 255;

#define MC_REGIMENT_COLUMNS(X)                                                                                  \
    /* its army's slot: a reference within one life, the army frees its regiments when it dies */               \
    X(regiments, std::uint16_t, army, by_hand)                                                                  \
    X(regiments, std::uint8_t, type, setup("type", key(&Content::regiment_types)))                              \
    /* men; a full regiment is 1000 (EU4's strength: CFixedPoint 1.0 = 1000 men) */                             \
    X(regiments, std::int32_t, strength, setup("strength", whole))                                              \
    /* Fixed; full at the start: the maximum morale of its army's owner (game/modifiers.h) */                   \
    X(regiments, Fixed, morale, derived)                                                                        \
    /* in its army's battle: kPlace* or a front position + 1 */                                                 \
    X(regiments, std::uint8_t, place, runtime)                                                                  \
    /* in its side's reserve, a list in EU4's order (BattleSide): the regiments before and after it, slot + 1, */ \
    /* 0 = none; both 0 out of the reserve */                                                                    \
    X(regiments, std::uint16_t, reserve_prev, runtime)                                                          \
    X(regiments, std::uint16_t, reserve_next, runtime)

    // Saved. The regiments of every army, in slots, kRegimentSlots long. data/setup/regiments.csv at the start: army
    // (its number in armies.csv) by hand.
    struct Regiments
    {
        MC_REGIMENT_COLUMNS(MC_DECLARE_COLUMN)
        Column<std::uint64_t> alive;   // by hand, like Armies::alive

        void resize();
    };

#define MC_LEADER_COLUMNS(X)                                                                                    \
    X(leaders, std::uint16_t, owner, setup("owner", country))                                                   \
    /* pips: EU4's leader fire, shock, maneuver and siege, 0..leader_max_pips (defines.csv) */                  \
    X(leaders, std::uint8_t, fire, setup("fire", whole))                                                        \
    X(leaders, std::uint8_t, shock, setup("shock", whole))                                                      \
    X(leaders, std::uint8_t, maneuver, setup("maneuver", whole))                                                \
    X(leaders, std::uint8_t, siege, setup("siege", whole))                                                      \
    /* grows each time the slot is freed (like Armies::generation) */                                           \
    X(leaders, std::uint16_t, generation, runtime)

    // Saved. The generals (land only, no admirals yet), in slots, kLeaderSlots long: a leader outlives its army, so it
    // is an entity of its own; the army names it (Armies::leader). data/setup/leaders.csv at the start: leader (its
    // number in the file) and army (its number in armies.csv, empty = none) by hand. No names yet, as with armies.
    struct Leaders
    {
        MC_LEADER_COLUMNS(MC_DECLARE_COLUMN)
        Column<std::uint64_t> alive;   // by hand, like Armies::alive

        void resize();
    };

    // A battle's front line: EU4's MAX_COMBAT_WIDTH positions + 1 (its lines are indexed 0..40). The width a battle
    // fights at is its countries' combat_width (game/modifiers.h), at most defines' max_combat_width.
    inline constexpr int kMaxCombatWidth = 40;
    inline constexpr int kFrontSlots = kMaxCombatWidth + 1;
    // Regiments::place of the back line's position p: kPlaceBack + p.
    inline constexpr std::uint8_t kPlaceBack = kFrontSlots + 1;
    static_assert(kPlaceBack + kFrontSlots <= kPlaceReserve, "the places of the two lines and the reserve differ");

    // A battle's phase (Battle::phase).
    inline constexpr std::uint8_t kPhaseFire = 0;
    inline constexpr std::uint8_t kPhaseShock = 1;
    // Battle::phase_day before the first day: the lines are not drawn up yet (EU4's _nDay = -1).
    inline constexpr std::uint8_t kNotDeployed = 255;

    // One side of a battle (Battle::sides). Regiments are named by slot + 1, 0 = none.
    struct BattleSide
    {
        std::uint16_t country = 0;   // no alliances yet: a country a side
        // The reserve (EU4's _Reserves): a list through Regiments::reserve_prev and reserve_next, in EU4's order (who
        // goes in goes to the end; the front takes the first fit one).
        std::uint16_t reserve_first = 0;
        std::uint16_t reserve_last = 0;
        std::uint16_t dice = 0;                           // the phase's roll, 0..dice_sides - 1
        std::array<std::uint16_t, kFrontSlots> front{};   // the front line, positions 0..kMaxCombatWidth
        std::array<std::uint16_t, kFrontSlots> back{};    // the back line behind it: artillery (EU4's _Back)
    };

    // A battle (game/combat.h). A structure, not columns: the battles are few and a battle's day touches all of it
    // (docs/SIMULATION.md, "Раскладка данных"). Integers only and no padding: the checksum hashes its bytes.
    struct Battle
    {
        std::array<std::int32_t, 2> joined{};   // the men who joined each side, for the winner's morale (EU4's
                                                // GiveEndOfCombatMoraleBoost)
        // The attacker's crossing (EU4's GetCrossingType before the maneuver): river_crossing_penalty if the army
        // that began the battle came into the province over a river (MapData::march_river), else 0.
        std::int32_t crossing = 0;
        std::array<BattleSide, 2> sides{};      // 0 the attacker (it came into the province), 1 the defender
        std::uint16_t province = 0;
        std::uint16_t duration = 0;     // days fought (EU4's _nDuration)
        std::uint16_t generation = 0;   // grows each time the slot is freed: with the slot, the battle's name for
                                        // its dice (core/random.h)
        std::uint8_t phase_day = 0;     // the day of the phase (EU4's _nDay), kNotDeployed before the first
        std::uint8_t phase = kPhaseFire;
    };
    static_assert(std::has_unique_object_representations_v<Battle>, "a battle's bytes are its value: no padding");

    // Saved. The battles going on, in slots, kBattleSlots long; runtime only (no setup file). A freed slot is
    // Battle{} but for its generation.
    struct Battles
    {
        Column<Battle> list;
        Column<std::uint64_t> alive;   // like Armies::alive

        void resize();
    };

#define MC_WAR_COLUMNS(X)                                                                                       \
    /* the country that declared it and the one it was declared on (no alliances yet: a country a side) */      \
    X(wars, std::uint16_t, attacker, runtime)                                                                   \
    X(wars, std::uint16_t, defender, runtime)                                                                   \
    /* the day it began (World::day) */                                                                         \
    X(wars, std::uint32_t, start, runtime)                                                                      \
    /* Fixed: the sum of its battles' scores (game/war_score.h), + for the attacker; not capped (the */         \
    /* warscore_max_from_battles cap is the war score's) */                                                     \
    X(wars, Fixed, battle_score, runtime)                                                                       \
    /* grows each time the slot is freed (like Armies::generation) */                                           \
    X(wars, std::uint16_t, generation, runtime)

    // Saved. The wars going on (game/war.h), in slots, kWarSlots long; runtime only (no setup file: no war at the
    // start). Two countries have one war between them at most.
    struct Wars
    {
        MC_WAR_COLUMNS(MC_DECLARE_COLUMN)
        Column<std::uint64_t> alive;   // by hand, like Armies::alive

        void resize();
    };

#define MC_TRUCE_COLUMNS(X)                                                                                     \
    /* the two countries that made peace, the lower index first */                                              \
    X(truces, std::uint16_t, first, runtime)                                                                    \
    X(truces, std::uint16_t, second, runtime)                                                                   \
    /* the first day they may declare war on each other again */                                                \
    X(truces, std::uint32_t, until, runtime)

    // Saved. The truces (game/war.h), in slots, kTruceSlots long; runtime only (no setup file). Two countries have
    // one at most; the month frees the ones that ran out.
    struct Truces
    {
        MC_TRUCE_COLUMNS(MC_DECLARE_COLUMN)
        Column<std::uint64_t> alive;   // by hand, like Armies::alive

        void resize();
    };

    // What the last phase of a siege came to (Sieges::result; EU4's ESiegeStatus, game/occupation.h).
    enum class SiegeResult : std::uint8_t
    {
        none = 0,           // no phase yet
        status_quo = 1,     // nothing
        disease = 2,        // the besiegers lose men
        breach = 3,         // the walls are breached
        supplies = 4,       // supplies shortage
        food = 5,           // food shortage
        water = 6,          // water shortage
        desertion = 7,      // the defenders desert
    };

#define MC_SIEGE_COLUMNS(X)                                                                                     \
    /* the province besieged (one siege a province at most) and the country besieging it (no alliances yet) */  \
    X(sieges, std::uint16_t, province, runtime)                                                                 \
    X(sieges, std::uint16_t, country, runtime)                                                                  \
    /* days into the current phase, and the phases done */                                                      \
    X(sieges, std::uint16_t, days, runtime)                                                                     \
    X(sieges, std::uint16_t, phases, runtime)                                                                   \
    /* the walls breached, 0..max_breach */                                                                     \
    X(sieges, std::uint8_t, breaches, runtime)                                                                  \
    /* the last phase's roll, 1..siege_dice_sides (0: none yet), and what it came to (SiegeResult) */           \
    X(sieges, std::uint8_t, roll, runtime)                                                                      \
    X(sieges, SiegeResult, result, runtime)                                                                     \
    /* grows each time the slot is freed (like Armies::generation): with the slot, the siege's name for its */  \
    /* dice (core/random.h) */                                                                                  \
    X(sieges, std::uint16_t, generation, runtime)

    // Saved. The sieges going on (game/occupation.h), in slots, kSiegeSlots long; runtime only (no setup file). The
    // siege's progress is the province's (Provinces::siege_progress: it outlives the siege), its armies are the
    // besieging country's that stand in the province.
    struct Sieges
    {
        MC_SIEGE_COLUMNS(MC_DECLARE_COLUMN)
        Column<std::uint64_t> alive;   // by hand, like Armies::alive

        void resize();
    };

    // The regiment types' categories (regiment_types.csv `category`): what a type is in battle.
    enum class RegimentCategory : std::uint8_t
    {
        infantry,
        cavalry,
        artillery,
    };
    inline constexpr std::string_view kRegimentCategories[] = { "infantry", "cavalry", "artillery" };

    // Content. The regiment types' numbers for battle (regiment_types.csv, a column each, whole numbers 0..255): EU4's
    // unit maneuver (how far aside from the regiment straight ahead it can strike: its flanking range; infantry 1,
    // cavalry 2) and its pips, offensive and defensive, of fire, shock and morale. One column per number over the
    // types, [0] (no type) 0.
#define MC_REGIMENT_TYPE_NUMBERS(X)                                                                             \
    X(maneuver)                                                                                                 \
    X(off_fire)                                                                                                 \
    X(def_fire)                                                                                                 \
    X(off_shock)                                                                                                \
    X(def_shock)                                                                                                \
    X(off_morale)                                                                                               \
    X(def_morale)
#define MC_DECLARE_TYPE_NUMBER(name) std::vector<std::uint8_t> name;
    struct RegimentTypeNumbers
    {
        MC_REGIMENT_TYPE_NUMBERS(MC_DECLARE_TYPE_NUMBER)
    };
#undef MC_DECLARE_TYPE_NUMBER

    // Content. The battle's constants (data/common/defines.csv: key, value; EU4's NDefines::NMilitary), every one
    // required: X(kind, name), whole a whole number >= 1 (int), count a whole number >= 0 (int), fixed a decimal
    // number (Fixed), real a decimal number (double: the economy's rates and shares, worked out in floating point).
#define MC_COMBAT_DEFINES(X)                                                                                    \
    /* days a fire or shock phase lasts; the dice are rolled anew with each phase */                            \
    X(whole, days_per_phase)                                                                                    \
    X(whole, dice_sides)                                                                                        \
    /* days before a side can lose on morale and retreat */                                                     \
    X(whole, cannot_retreat_days)                                                                               \
    /* how many times stronger a side wipes the other out: at the end, and while it cannot be left */           \
    X(whole, overrun_factor)                                                                                    \
    X(whole, overrun_factor_cannot_leave)                                                                       \
    /* the widest a front line can be, at most kMaxCombatWidth */                                               \
    X(whole, max_combat_width)                                                                                  \
    /* the damage grows by this each day of the battle */                                                       \
    X(fixed, war_length_damage)                                                                                 \
    /* a day's morale loss in the line and in the reserve: these × the enemy's maximum morale */                \
    X(fixed, daily_morale_damage)                                                                               \
    X(fixed, reserve_daily_morale_damage)                                                                       \
    X(fixed, front_line_modifier)                                                                               \
    /* the most pips a general has in each of fire, shock, maneuver and siege (EU4's LEADER_MAX_PIPS) */     \
    X(whole, leader_max_pips)                                                                                   \
    /* the back line (artillery): its shots' range (the front line's is front_line_modifier), the share of a hit */ \
    /* on the regiment in front of it that it takes, in morale and in men; how many regiments a day go into it */ \
    /* (+ the commander's maneuver / 2) */                                                                       \
    X(fixed, back_line_modifier)                                                                                \
    X(fixed, back_line_morale_damage_taken)                                                                     \
    X(fixed, back_line_strength_damage_taken)                                                                   \
    X(whole, back_line_reinforce_speed)                                                                         \
    /* the attacker's dice minus when it came into the province over a river (EU4's river crossing; no straits) */ \
    X(whole, river_crossing_penalty)                                                                            \
    /* out of battle, each month: the share of the maximum a regiment gets back (morale; men too, M8) on its */ \
    /* country's land, on no one's or occupied land, on an enemy's, and while it retreats (game/recovery.h) */  \
    X(fixed, recovery_own_land)                                                                                 \
    X(fixed, recovery_neutral_land)                                                                             \
    X(fixed, recovery_enemy_land)                                                                               \
    X(fixed, recovery_retreating)                                                                               \
    /* the siege (game/occupation.h; EU4's names): the days of a phase before the modifiers; the siege's die, */ \
    /* 1..siege_dice_sides (EU4 14: a breach needs this much); the roll + impact that makes the fort surrender; */ \
    /* the most breaches; the progress's cap, siege_memory + siege_memory_scaled × the besieger's highest_fort */ \
    X(whole, days_per_siege_phase)                                                                              \
    X(whole, siege_dice_sides)                                                                                  \
    X(whole, siege_win)                                                                                         \
    X(whole, max_breach)                                                                                        \
    X(whole, siege_memory)                                                                                      \
    X(fixed, siege_memory_scaled)                                                                               \
    /* the progress a result gives: supplies, food and water shortage, the defenders desert */                  \
    X(whole, siege_bonus_supplies_shortage)                                                                     \
    X(whole, siege_bonus_food_shortage)                                                                         \
    X(whole, siege_bonus_water_shortage)                                                                        \
    X(whole, siege_bonus_defenders_desert)                                                                      \
    /* the share of their men the besiegers lose to a disease (a roll of 1) */                                  \
    X(fixed, siege_disease_impact)                                                                              \
    /* below this many men the garrison surrenders at the next phase */                                         \
    X(whole, siege_garrison_surrender)                                                                          \
    /* the men of the garrison a fort level; the besiegers need this × the maximum garrison, and never fewer */ \
    /* than siege_min_men (the user's decision: 1000, also where there is no garrison) */                       \
    X(whole, garrison_size)                                                                                     \
    X(fixed, siege_force_needed_multiplier)                                                                     \
    X(whole, siege_min_men)                                                                                     \
    /* the fort levels a capital has on top of its fort (EU4 1; 0 = none) */                                    \
    X(count, capital_fort_level)                                                                                \
    /* the impact's minus at a fort with a port, not blockaded (there are no fleets yet: every such fort) */    \
    X(count, siege_unblockaded_penalty)                                                                         \
    /* the garrison of a fort just taken (EU4 100), and its growth a month out of a siege, of the maximum */    \
    X(count, garrison_after_capture)                                                                            \
    X(fixed, garrison_growth)
    // Content. The war's and the peace's constants (data/common/defines.csv too: EU4's NDiplomacy and NMilitary), the
    // same kinds as the battle's, every one required.
#define MC_WAR_DEFINES(X)                                                                                       \
    /* the most a war score is (EU4's MAX_WARSCORE), the most its battles give (WARSCORE_MAX_FROM_BATTLES) */   \
    X(whole, max_warscore)                                                                                      \
    X(whole, warscore_max_from_battles)                                                                         \
    /* a province's war value (game/war_score.h): its base, the cap of its development (AE_PROVINCE_CAP), */    \
    /* the capital's multiplier (PEACE_COST_DEMAND_CAPITAL_MULT), the development a fort answers for */         \
    /* (FORT_PER_DEV_RATIO) */                                                                                  \
    X(fixed, province_value_base)                                                                               \
    X(fixed, province_value_cap)                                                                                \
    X(fixed, capital_value_mult)                                                                                \
    X(fixed, fort_per_dev_ratio)                                                                                \
    /* the years of a truce after a peace (EU4's TRUCE_YEARS) */                                                \
    X(whole, truce_years)
    // Content. The economy's constants (data/common/defines.csv too; EU3's, docs/ECONOMY.md), the same kinds, every
    // one required (game/economy/income.h, budget.h).
#define MC_ECONOMY_DEFINES(X)                                                                                   \
    /* what a capital adds to its base_tax, in the tax and the census (EU3 2) */                                \
    X(real, capital_tax)                                                                                        \
    /* the census's share of a province with its owner's core (EU3 100%) and of one without (10%) */            \
    X(real, census_core)                                                                                        \
    X(real, census_no_core)                                                                                     \
    /* the budget sliders at the start, whole percents adding up to 100 (the user's decision: 25 each, */       \
    /* Treasury 0) */                                                                                           \
    X(count, budget_army)                                                                                       \
    X(count, budget_production)                                                                                 \
    X(count, budget_government)                                                                                 \
    X(count, budget_stability)                                                                                  \
    X(count, budget_treasury)                                                                                   \
    /* the inflation a year of the whole monthly income in the Treasury gives, in percent (EU3 1:100: 60% */    \
    /* in the Treasury, +0.6 a year) */                                                                         \
    X(real, treasury_inflation)                                                                                 \
    /* a province's volume of its good a year (EU3 In Nomine): production_base + population / */                \
    /* population_per_unit + base_tax / base_tax_per_unit (0.99, 100 000, 20) */                                \
    X(real, production_base)                                                                                    \
    X(whole, population_per_unit)                                                                               \
    X(whole, base_tax_per_unit)                                                                                 \
    /* the land force limit (game/economy/upkeep.h; the user's decision): force_limit_base for every country */ \
    /* + force_limit_per_base_tax × (its provinces' base_tax + capital_tax at its seat) (6, EU3's 0.25) */      \
    X(real, force_limit_base)                                                                                   \
    X(real, force_limit_per_base_tax)                                                                           \
    /* a regiment's upkeep a month: its type's cost × this × its strength / 1000 (EU3 2.5%); over the force */  \
    /* limit × (1 + force_limit_extra_cost × (regiments - limit) / limit) (EU3 1: in proportion; EU4 2) */      \
    X(real, land_maintenance)                                                                                   \
    X(real, force_limit_extra_cost)                                                                             \
    /* the land maintenance slider, whole percents: its lowest (the upkeep × it) and its start; at the lowest */ \
    /* the regiments' morale ceiling is min_maintenance_morale of the maximum, at 100 the maximum, linear */     \
    /* between (the user's decision: 50, 100, 0.2) */                                                           \
    X(whole, maintenance_min)                                                                                   \
    X(whole, maintenance_start)                                                                                 \
    X(fixed, min_maintenance_morale)

#define MC_DEFINE_TYPE_whole int
#define MC_DEFINE_TYPE_count int
#define MC_DEFINE_TYPE_fixed Fixed
#define MC_DEFINE_TYPE_real double
#define MC_DECLARE_DEFINE(kind, name) MC_DEFINE_TYPE_##kind name = 0;
    struct CombatDefines
    {
        MC_COMBAT_DEFINES(MC_DECLARE_DEFINE)
    };
    struct WarDefines
    {
        MC_WAR_DEFINES(MC_DECLARE_DEFINE)
    };
    struct EconomyDefines
    {
        MC_ECONOMY_DEFINES(MC_DECLARE_DEFINE)
    };
#undef MC_DECLARE_DEFINE

    // The money good: worth 1 emerald whatever goods.csv says (docs/GAMEDATA.md, "Файлы").
    inline constexpr std::string_view kMoneyGood = "emerald";

    // Content: what the game is made of (data/common and the map): lists and definitions, no state. Fixed once
    // loaded; strings live here and only here (UI, editor, logs), never in WorldState.
    struct Content
    {
        DefinitionList terrain;
        std::vector<float> terrain_movement_cost;   // terrain.csv `movement_cost`: a march's multiplier over it (the
                                                    // plains 1; game/movement.h); [0] (no terrain) 1
        std::vector<std::uint8_t> terrain_attacker_penalty;   // terrain.csv `attacker_penalty`: the dice's minus of
                                                              // an attacker into it (game/combat.h); [0] 0
        DefinitionList goods;
        std::vector<float> goods_price;   // goods.csv `price`: the base price, emeralds per unit produced (the money
                                          // good: 1); production sells at it (game/economy/income.h)
        std::uint8_t money_good = 0;      // the good kMoneyGood (EU3's gold: no price, straight into the income),
                                          // 0 = none
        DefinitionList trade_nodes;       // data/common/trade_nodes.csv: a node is made of whole regions (M11 rules)
        DefinitionList regiment_types;    // data/common/regiment_types.csv: the units (EU4's Latin Knights...)
        std::vector<RegimentCategory> regiment_category;   // per type
        RegimentTypeNumbers regiment_numbers;              // per type: maneuver and pips
        std::vector<float> regiment_cost;                  // per type (regiment_types.csv `cost`): what a regiment
                                                           // costs to raise, emeralds (EU3 9, 21, 30); its upkeep
                                                           // follows (game/economy/upkeep.h); [0] 0
        CombatDefines combat;             // data/common/defines.csv
        WarDefines war;                   // data/common/defines.csv too
        EconomyDefines economy;           // and these
        ModifierBundles modifiers;        // data/common/modifiers.csv
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
        Leaders leaders;
        Battles battles;
        Wars wars;
        Sieges sieges;
        Truces truces;
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
        // countries.csv, armies.csv, regiments.csv and leaders.csv (the live ones in slot order, armies and leaders
        // numbered 1, 2, ... in it)
        // from `state`, each in its `setup_layout` order.
        bool save_setup(const std::filesystem::path& dir, const MapData& map, std::string* error) const;
        // The checks that are warnings at load, for the current state (and its cache): a capital its country does not
        // own; a country whose discipline or maximum morale is not above 0; a land province without a good; an army without an owner or regiments, a regiment without a type; once there are
        // trade nodes, a region without one and a node without regions.
        [[nodiscard]] std::vector<std::string> check(const MapData& map) const;
        // "11 countries, 5 terrain, ..." for the logs.
        [[nodiscard]] std::string summary() const;
        // The cache from the content and the state (load does it; then a restart and every edit of a source).
        void rebuild_cache();

        bool loaded = false;
        Content content;
        WorldState state;
        Cache cache;
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
            std::vector<std::string> leaders;
        } setup_layout;
    };
}
