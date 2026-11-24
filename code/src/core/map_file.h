#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <span>
#include <string>
#include <string_view>
#include <vector>

// world.mcmap's container (docs/MAPFORMAT.md): the header, the table of sections, and reading only the sections a
// part of the map asks for. The simulation's part (core/map_data.h) and the view's (view/map_view.h) read their own.

namespace mc
{
    // The bytes of the sections read, one buffer each (operator new aligns them to 16, as the file does), so the
    // spans into them stay put while more are read.
    using MapSections = std::vector<std::vector<std::byte>>;

    template <class T>
    [[nodiscard]] std::span<const T> as_span(std::span<const std::byte> bytes)
    {
        return { reinterpret_cast<const T*>(bytes.data()), bytes.size() / sizeof(T) };
    }

    // A string table written by bake.py `strings`: u32 offsets (one more than strings) and a UTF-8 blob.
    struct MapStrings
    {
        std::span<const std::uint32_t> offsets;
        std::span<const char> blob;

        [[nodiscard]] std::string_view at(std::size_t i) const;
    };

    // Reads sections of one world.mcmap into a MapSections. Every check records only the first problem; finish()
    // fails with it. The file is ours (bake.py), so a problem means a stale or broken bake: the message names the
    // section and asks for a rebuild.
    class MapReader
    {
    public:
        // The header and the section table: every entry inside the file and 16-aligned.
        bool open(const std::filesystem::path& path, MapSections* store, std::string* error);

        [[nodiscard]] std::uint32_t width() const { return width_; }
        [[nodiscard]] std::uint32_t height() const { return height_; }
        [[nodiscard]] std::uint32_t count() const { return count_; }

        // A section of exactly `bytes` bytes.
        std::span<const std::byte> exact(const char* tag, std::size_t bytes);
        // A section of whole elements of `unit` bytes (it may be empty only where `may_be_empty`).
        std::span<const std::byte> whole(const char* tag, std::size_t unit, bool may_be_empty);
        // A string table of `strings` strings: offsets rising from 0, the last within the blob.
        MapStrings strings(const char* tag, std::size_t strings);

        void bad(std::string message);
        [[nodiscard]] bool ok() const { return problem_.empty(); }
        // False, with the first problem in `error`, if any check failed.
        bool finish(std::string* error) const;

    private:
        struct Section
        {
            char tag[4];
            std::uint32_t size;
            std::uint64_t offset;
        };
        static_assert(sizeof(Section) == 16);

        // The section's bytes, read into the store; empty if the file has no such section.
        std::span<const std::byte> read(const char* tag);

        std::filesystem::path path_;
        std::ifstream in_;
        MapSections* store_ = nullptr;
        std::vector<Section> sections_;
        std::uint32_t width_ = 0;
        std::uint32_t height_ = 0;
        std::uint32_t count_ = 0;
        std::string problem_;
    };
}
