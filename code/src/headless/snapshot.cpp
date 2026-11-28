#include "headless/snapshot.h"

#include <cstring>
#include <format>
#include <span>
#include <string_view>
#include <type_traits>

#include "game/modifiers.h"
#include "game/saved_state.h"
#include "game/world.h"

namespace mc::headless
{
    namespace
    {
        class Hash
        {
        public:
            void add(std::span<const std::byte> bytes)
            {
                std::size_t i = 0;
                for (; i + 8 <= bytes.size(); i += 8) {
                    std::uint64_t word = 0;
                    std::memcpy(&word, bytes.data() + i, 8);
                    mix(word);
                }
                if (i < bytes.size()) {
                    std::uint64_t word = 0;
                    std::memcpy(&word, bytes.data() + i, bytes.size() - i);
                    mix(word);
                }
            }
            template <class T>
            void add_value(const T& value)
            {
                add(std::as_bytes(std::span(&value, 1)));
            }
            [[nodiscard]] std::uint64_t value() const { return hash_; }

        private:
            void mix(std::uint64_t word)
            {
                hash_ = (hash_ ^ word) * 0x100000001b3ull;
                hash_ ^= hash_ >> 29;
            }

            std::uint64_t hash_ = 0xcbf29ce484222325ull;
        };
    }

    std::uint64_t checksum(const World& world)
    {
        Hash hash;
        for_each_saved(world.state(), [&](std::string_view name, auto column) {
            hash.add(std::as_bytes(std::span(name)));
            hash.add_value(static_cast<std::uint64_t>(column.size()));
            hash.add(std::as_bytes(column));
        });
        hash.add_value(world.day());
        hash.add_value(world.seed());
        return hash.value();
    }

    namespace
    {
        // The battles, a structure (game/game_data.h, Battle): rows battles.<field>,slot,value; a side's field is
        // battles.<attacker|defender>.<field>, its lines battles.<side>.front and battles.<side>.back,
        // slot × kFrontSlots + position,value.
        void dump_battles(std::span<const Battle> battles, std::string* out)
        {
            static_assert(sizeof(Battle) == 364, "a new field of Battle: dump it here");
            constexpr std::string_view kSides[] = { "attacker", "defender" };
            for (std::size_t b = 0; b < battles.size(); ++b) {
                const Battle& battle = battles[b];
                *out += std::format("battles.province,{},{}\n", b, battle.province);
                *out += std::format("battles.duration,{},{}\n", b, battle.duration);
                *out += std::format("battles.phase_day,{},{}\n", b, battle.phase_day);
                *out += std::format("battles.phase,{},{}\n", b, battle.phase);
                *out += std::format("battles.generation,{},{}\n", b, battle.generation);
                *out += std::format("battles.crossing,{},{}\n", b, battle.crossing);
                for (std::size_t s = 0; s < 2; ++s) {
                    const BattleSide& side = battle.sides[s];
                    const std::string_view n = kSides[s];
                    *out += std::format("battles.{}.joined,{},{}\n", n, b, battle.joined[s]);
                    *out += std::format("battles.{}.country,{},{}\n", n, b, side.country);
                    *out += std::format("battles.{}.dice,{},{}\n", n, b, side.dice);
                    *out += std::format("battles.{}.reserve_first,{},{}\n", n, b, side.reserve_first);
                    *out += std::format("battles.{}.reserve_last,{},{}\n", n, b, side.reserve_last);
                    for (std::size_t p = 0; p < side.front.size(); ++p) {
                        *out += std::format("battles.{}.front,{},{}\n", n, b * side.front.size() + p, side.front[p]);
                    }
                    for (std::size_t p = 0; p < side.back.size(); ++p) {
                        *out += std::format("battles.{}.back,{},{}\n", n, b * side.back.size() + p, side.back[p]);
                    }
                }
            }
        }

        // Rows column,index,value for every element of `column`.
        template <class T>
        void dump_column(std::string_view name, std::span<T> column, std::string* out)
        {
            using V = std::remove_cv_t<T>;
            if constexpr (std::is_same_v<V, Battle>) {
                dump_battles(column, out);
            } else {
                for (std::size_t i = 0; i < column.size(); ++i) {
                    if constexpr (std::is_floating_point_v<V>) {
                        *out += std::format("{},{},{}\n", name, i, column[i]);
                    } else if constexpr (std::is_signed_v<V>) {
                        *out += std::format("{},{},{}\n", name, i, static_cast<std::int64_t>(column[i]));
                    } else {
                        *out += std::format("{},{},{}\n", name, i, static_cast<std::uint64_t>(column[i]));
                    }
                }
            }
        }
    }

    std::string dump(const World& world)
    {
        std::string out = "column,index,value\n";
        out += std::format("day,0,{}\n", world.day());
        out += std::format("seed,0,{}\n", world.seed());
        for_each_saved(world.state(), [&](std::string_view name, auto column) { dump_column(name, column, &out); });
        return out;
    }

    std::string dump_cache(const World& world)
    {
        std::string out = "column,index,value\n";
        for_each_cached(world.cache(), [&](std::string_view name, auto column) { dump_column(name, column, &out); });
        return out;
    }
}
