#include "core/map_file.h"

#include <algorithm>
#include <cstring>
#include <format>

namespace mc
{
    namespace
    {
        struct Header
        {
            char magic[4];
            std::uint32_t version;
            std::uint32_t width;
            std::uint32_t height;
            std::uint32_t count;
            std::uint32_t sections;
        };
        static_assert(sizeof(Header) == 24);
    }

    std::string_view MapStrings::at(std::size_t i) const
    {
        if (i + 1 >= offsets.size()) {
            return {};
        }
        const std::uint32_t begin = offsets[i];
        const std::uint32_t end = offsets[i + 1];
        if (begin > end || end > blob.size()) {
            return {};
        }
        return { blob.data() + begin, end - begin };
    }

    bool MapReader::open(const std::filesystem::path& path, MapSections* store, std::string* error)
    {
        path_ = path;
        store_ = store;
        auto fail = [&](std::string message) {
            if (error) {
                *error = std::format("{}: {}", path.string(), message);
            }
            return false;
        };

        in_.open(path, std::ios::binary | std::ios::ate);
        if (!in_) {
            return fail("cannot open");
        }
        const auto file_size = static_cast<std::uint64_t>(in_.tellg());
        in_.seekg(0);
        Header header;
        if (file_size < sizeof(Header) || !in_.read(reinterpret_cast<char*>(&header), sizeof(header))) {
            return fail("cannot read");
        }
        if (std::memcmp(header.magic, "MCMP", 4) != 0 || header.version != 4) {
            return fail("not an MCMP v4 file (rebuild with tools/mapbake/bake.py)");
        }
        if (file_size < sizeof(Header) + std::uint64_t{ header.sections } * sizeof(Section)) {
            return fail("truncated section table");
        }
        if (header.width == 0 || header.height == 0 || header.count < 2 || header.count > 0x10000) {
            return fail(std::format("bad header: {}x{} px, {} province slots (rebuild with tools/mapbake/bake.py)",
                                    header.width, header.height, header.count));
        }
        width_ = header.width;
        height_ = header.height;
        count_ = header.count;

        sections_.resize(header.sections);
        if (!in_.read(reinterpret_cast<char*>(sections_.data()),
                      static_cast<std::streamsize>(sections_.size() * sizeof(Section)))) {
            return fail("cannot read the section table");
        }
        // The table's entries must lie inside the file, 16-aligned (the spans are cast in place).
        for (const Section& s : sections_) {
            if (s.offset % 16 != 0 || s.offset > file_size || s.size > file_size - s.offset) {
                return fail(std::format("malformed map: section {} lies outside the file or is misaligned "
                                        "(rebuild with tools/mapbake/bake.py)",
                                        std::string_view(s.tag, 4)));
            }
        }
        return true;
    }

    std::span<const std::byte> MapReader::read(const char* tag)
    {
        const auto s =
            std::ranges::find_if(sections_, [&](const Section& e) { return std::memcmp(e.tag, tag, 4) == 0; });
        if (s == sections_.end() || s->size == 0) {
            return {};
        }
        std::vector<std::byte>& bytes = store_->emplace_back(s->size);
        in_.clear();
        in_.seekg(static_cast<std::streamoff>(s->offset));
        if (!in_.read(reinterpret_cast<char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()))) {
            bad(std::format("section {} cannot be read", tag));
            return {};
        }
        return bytes;
    }

    std::span<const std::byte> MapReader::exact(const char* tag, std::size_t bytes)
    {
        const auto s = read(tag);
        if (s.size() != bytes) {
            bad(std::format("section {} has {} bytes, expected {}", tag, s.size(), bytes));
        }
        return s;
    }

    std::span<const std::byte> MapReader::whole(const char* tag, std::size_t unit, bool may_be_empty)
    {
        const auto s = read(tag);
        if (s.size() % unit != 0 || (s.empty() && !may_be_empty)) {
            bad(std::format("section {} has {} bytes, not a{} multiple of {}", tag, s.size(),
                            may_be_empty ? "" : " non-empty", unit));
        }
        return s;
    }

    MapStrings MapReader::strings(const char* tag, std::size_t strings)
    {
        const auto s = read(tag);
        const std::size_t head = (strings + 1) * sizeof(std::uint32_t);
        if (s.size() < head) {
            bad(std::format("section {} has {} bytes, too few for {} strings", tag, s.size(), strings));
            return {};
        }
        const auto offsets = as_span<std::uint32_t>(s.first(head));
        const auto blob = as_span<char>(s.subspan(head));
        if (offsets[0] != 0 || !std::ranges::is_sorted(offsets) || offsets.back() > blob.size()) {
            bad(std::format("section {}: string offsets out of order or past the end", tag));
            return {};
        }
        return { offsets, blob };
    }

    void MapReader::bad(std::string message)
    {
        if (problem_.empty()) {
            problem_ = std::move(message);
        }
    }

    bool MapReader::finish(std::string* error) const
    {
        if (ok()) {
            return true;
        }
        if (error) {
            *error = std::format("{}: malformed map: {} (rebuild with tools/mapbake/bake.py)", path_.string(),
                                 problem_);
        }
        return false;
    }
}
