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
    X(countries, std::uint16_t, capital, by_hand)

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
    X(cavalry_shock)

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

    // Cache: the rules' data rebuilt from Content and WorldState (deterministically: CLAUDE.md, rule 9), never saved.
    // for_each_cached (game/modifiers.h) walks it for mapcraft_sim's cache dump.
    struct Cache
    {
        CountryModifiers country_modifiers;
    };

    // Entities that are born and die (armies, regiments, leaders, battles) live in slots: a fixed number per table, so
    // the tick never allocates; a mask of the slots in use; a new entity takes the lowest free slot (game/armies.h). A
    // freed slot's columns are back to 0. The numbers are the limits: regiments fill the u16 slot index but for the
    // last word (a battle names a regiment by its slot + 1 in a u16, 0 = none: Battle, Regiments::reserve_prev and
    // reserve_next; more needs u32 slots), which is far above what EU4 fields on a map six times ours; a battle is one
    // per province at most; EU4's countries keep a few leaders each.
    inline constexpr std::uint32_t kArmySlots = 4096;
    inline constexpr std::uint32_t kRegimentSlots = 65472;
    inline constexpr std::uint32_t kLeaderSlots = 1024;
    inline constexpr std::uint32_t kBattleSlots = 1024;
    static_assert(kArmySlots % 64 == 0 && kRegimentSlots % 64 == 0 && kLeaderSlots % 64 == 0 &&
                      kBattleSlots % 64 == 0,
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
    // front line + 1 (EU4's 0..MAX_COMBAT_WIDTH), in the reserve, or out of the lines (broken: still in its army,
    // still counted, never back in this battle).
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
    /* pips: EU4's leader fire, shock and maneuver, 0..leader_max_pips (defines.csv; siege comes with the siege) */ \
    X(leaders, std::uint8_t, fire, setup("fire", whole))                                                        \
    X(leaders, std::uint8_t, shock, setup("shock", whole))                                                      \
    X(leaders, std::uint8_t, maneuver, setup("maneuver", whole))                                                \
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
    };

    // A battle (game/combat.h). A structure, not columns: the battles are few and a battle's day touches all of it
    // (docs/SIMULATION.md, "Раскладка данных"). Integers only and no padding: the checksum hashes its bytes.
    struct Battle
    {
        std::array<std::int32_t, 2> joined{};   // the men who joined each side, for the winner's morale (EU4's
                                                // GiveEndOfCombatMoraleBoost)
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
    // required: X(kind, name), whole a whole number >= 1 (int), fixed a decimal number (Fixed).
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
    /* the most pips a general has in each of fire, shock and maneuver (EU4's LEADER_MAX_PIPS) */               \
    X(whole, leader_max_pips)
#define MC_DEFINE_TYPE_whole int
#define MC_DEFINE_TYPE_fixed Fixed
#define MC_DECLARE_DEFINE(kind, name) MC_DEFINE_TYPE_##kind name = 0;
    struct CombatDefines
    {
        MC_COMBAT_DEFINES(MC_DECLARE_DEFINE)
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
        DefinitionList goods;
        std::vector<float> goods_price;   // goods.csv `price`: the base price, emeralds per unit produced (the money
                                          // good: 1); no rule reads it yet
        DefinitionList trade_nodes;       // data/common/trade_nodes.csv: a node is made of whole regions (M11 rules)
        DefinitionList regiment_types;    // data/common/regiment_types.csv: the units (EU4's Latin Knights...)
        std::vector<RegimentCategory> regiment_category;   // per type
        RegimentTypeNumbers regiment_numbers;              // per type: maneuver and pips
        CombatDefines combat;             // data/common/defines.csv
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
