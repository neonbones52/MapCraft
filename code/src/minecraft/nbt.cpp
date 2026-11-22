#include "minecraft/nbt.h"

#include <bit>
#include <cstring>
#include <format>

#include <miniz.h>

namespace mc::minecraft
{
    namespace
    {
        constexpr int kMaxDepth = 512;

        class Reader
        {
        public:
            explicit Reader(std::span<const std::uint8_t> data) : data_(data) {}

            bool tag(Tag::Type type, Tag* out, int depth)
            {
                if (depth > kMaxDepth) {
                    return false;
                }
                out->type = type;
                switch (type) {
                case Tag::Type::byte: return number<std::int8_t>(out);
                case Tag::Type::short_: return number<std::int16_t>(out);
                case Tag::Type::int_: return number<std::int32_t>(out);
                case Tag::Type::long_: return number<std::int64_t>(out);
                case Tag::Type::float_: {
                    std::uint32_t bits = 0;
                    if (!big(&bits)) {
                        return false;
                    }
                    out->real = std::bit_cast<float>(bits);
                    return true;
                }
                case Tag::Type::double_: {
                    std::uint64_t bits = 0;
                    if (!big(&bits)) {
                        return false;
                    }
                    out->real = std::bit_cast<double>(bits);
                    return true;
                }
                case Tag::Type::byte_array: return array(Tag::Type::byte, out, depth);
                case Tag::Type::int_array: return array(Tag::Type::int_, out, depth);
                case Tag::Type::long_array: return array(Tag::Type::long_, out, depth);
                case Tag::Type::string: return string(&out->text);
                case Tag::Type::list: {
                    std::uint8_t element = 0;
                    std::int32_t count = 0;
                    if (!big(&element) || !big(&count) || count < 0 || element > 12) {
                        return false;
                    }
                    out->items.resize(static_cast<std::size_t>(count));
                    for (Tag& item : out->items) {
                        if (!tag(static_cast<Tag::Type>(element), &item, depth + 1)) {
                            return false;
                        }
                    }
                    return true;
                }
                case Tag::Type::compound:
                    for (;;) {
                        std::uint8_t field = 0;
                        if (!big(&field) || field > 12) {
                            return false;
                        }
                        if (field == 0) {
                            return true;
                        }
                        std::string name;
                        Tag value;
                        if (!string(&name) || !tag(static_cast<Tag::Type>(field), &value, depth + 1)) {
                            return false;
                        }
                        out->fields.emplace_back(std::move(name), std::move(value));
                    }
                default: return false;
                }
            }

            template <class T>
            bool big(T* out)
            {
                if (at_ + sizeof(T) > data_.size()) {
                    return false;
                }
                std::uint8_t bytes[sizeof(T)];
                for (std::size_t i = 0; i < sizeof(T); ++i) {
                    bytes[i] = data_[at_ + sizeof(T) - 1 - i];
                }
                std::memcpy(out, bytes, sizeof(T));
                at_ += sizeof(T);
                return true;
            }

            bool string(std::string* out)
            {
                std::uint16_t length = 0;
                if (!big(&length) || at_ + length > data_.size()) {
                    return false;
                }
                out->assign(reinterpret_cast<const char*>(data_.data() + at_), length);   // modified UTF-8, as is
                at_ += length;
                return true;
            }

        private:
            template <class T>
            bool number(Tag* out)
            {
                T value{};
                if (!big(&value)) {
                    return false;
                }
                out->integer = value;
                out->real = static_cast<double>(value);
                return true;
            }

            bool array(Tag::Type element, Tag* out, int depth)
            {
                std::int32_t count = 0;
                if (!big(&count) || count < 0) {
                    return false;
                }
                out->items.resize(static_cast<std::size_t>(count));
                for (Tag& item : out->items) {
                    if (!tag(element, &item, depth + 1)) {
                        return false;
                    }
                }
                return true;
            }

            std::span<const std::uint8_t> data_;
            std::size_t at_ = 0;
        };

        // A gzip member's deflate data (RFC 1952) -> the uncompressed bytes.
        bool gunzip(std::span<const std::uint8_t> file, std::vector<std::uint8_t>* out)
        {
            if (file.size() < 18 || file[0] != 0x1F || file[1] != 0x8B || file[2] != 8) {
                return false;
            }
            const std::uint8_t flags = file[3];
            std::size_t at = 10;
            if (flags & 4) {   // FEXTRA
                if (at + 2 > file.size()) {
                    return false;
                }
                at += 2 + (file[at] | (file[at + 1] << 8));
            }
            for (const std::uint8_t text : { std::uint8_t{ 8 }, std::uint8_t{ 16 } }) {   // FNAME, FCOMMENT
                if (flags & text) {
                    while (at < file.size() && file[at] != 0) {
                        ++at;
                    }
                    ++at;
                }
            }
            if (flags & 2) {   // FHCRC
                at += 2;
            }
            if (at + 8 > file.size()) {
                return false;
            }
            const std::size_t tail = file.size() - 4;
            const std::size_t size = file[tail] | (file[tail + 1] << 8) | (file[tail + 2] << 16) |
                                     (static_cast<std::size_t>(file[tail + 3]) << 24);
            out->resize(size);
            const std::size_t got =
                tinfl_decompress_mem_to_mem(out->data(), out->size(), file.data() + at, file.size() - 8 - at, 0);
            return got == size;
        }
    }

    const Tag* Tag::get(std::string_view name) const
    {
        for (const auto& [key, value] : fields) {
            if (key == name) {
                return &value;
            }
        }
        return nullptr;
    }

    bool read_nbt(std::span<const std::uint8_t> file, Tag* root, std::string* error)
    {
        std::vector<std::uint8_t> data;
        if (!gunzip(file, &data)) {
            if (error) {
                *error = "not a gzip file";
            }
            return false;
        }
        Reader reader(data);
        std::uint8_t type = 0;
        std::string name;
        *root = Tag{};
        if (!reader.big(&type) || type != static_cast<std::uint8_t>(Tag::Type::compound) || !reader.string(&name) ||
            !reader.tag(Tag::Type::compound, root, 0)) {
            if (error) {
                *error = "malformed NBT";
            }
            return false;
        }
        return true;
    }
}
