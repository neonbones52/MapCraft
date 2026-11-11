#pragma once

#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

namespace mc::csv
{
    // A CSV file (RFC 4180: quoted fields may hold commas, quotes as "", line breaks), UTF-8, header first.
    struct Table
    {
        std::vector<std::string> header;
        std::vector<std::vector<std::string>> rows;   // every row has header.size() fields (missing ones empty)
        std::vector<int> lines;                       // file line of each row, for messages

        // Column index by header name, -1 if absent.
        [[nodiscard]] int column(std::string_view name) const;
    };

    bool read(const std::filesystem::path& path, Table* table, std::string* error);

    // A field as written: quoted only when it has to be.
    [[nodiscard]] std::string field(std::string_view text);

    // Writes `content` next to `path` and renames it over, so a crash never leaves half a file.
    bool write_file(const std::filesystem::path& path, const std::string& content, std::string* error);
}
