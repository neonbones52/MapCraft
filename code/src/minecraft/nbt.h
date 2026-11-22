#pragma once

#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace mc::minecraft
{
    // A tag of Minecraft's Named Binary Tag format (big-endian), as structure files (.nbt) store it. Numbers keep
    // their kind in `type`; arrays and lists hold their elements in `items`.
    struct Tag
    {
        enum class Type : std::uint8_t
        {
            end = 0,
            byte = 1,
            short_ = 2,
            int_ = 3,
            long_ = 4,
            float_ = 5,
            double_ = 6,
            byte_array = 7,
            string = 8,
            list = 9,
            compound = 10,
            int_array = 11,
            long_array = 12,
        };

        Type type = Type::end;
        std::int64_t integer = 0;
        double real = 0.0;
        std::string text;
        std::vector<Tag> items;                              // list and array elements
        std::vector<std::pair<std::string, Tag>> fields;     // compound

        // A compound's field, or null.
        [[nodiscard]] const Tag* get(std::string_view name) const;
        [[nodiscard]] bool is_number() const { return type >= Type::byte && type <= Type::double_; }
    };

    // A gzip-compressed NBT file (Minecraft's structure files) -> its root tag.
    bool read_nbt(std::span<const std::uint8_t> file, Tag* root, std::string* error);
}
