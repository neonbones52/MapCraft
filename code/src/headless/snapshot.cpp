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
        return hash.value();
    }

    namespace
    {
        // Rows column,index,value for every element of `column`.
        template <class T>
        void dump_column(std::string_view name, std::span<T> column, std::string* out)
        {
            using V = std::remove_cv_t<T>;
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

    std::string dump(const World& world)
    {
        std::string out = "column,index,value\n";
        out += std::format("day,0,{}\n", world.day());
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
