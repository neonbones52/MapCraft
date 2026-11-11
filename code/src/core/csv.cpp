#include "core/csv.h"

#include <format>
#include <fstream>
#include <sstream>

namespace mc::csv
{
    int Table::column(std::string_view name) const
    {
        for (std::size_t i = 0; i < header.size(); ++i) {
            if (header[i] == name) {
                return static_cast<int>(i);
            }
        }
        return -1;
    }

    bool read(const std::filesystem::path& path, Table* table, std::string* error)
    {
        std::ifstream in(path, std::ios::binary);
        if (!in) {
            *error = std::format("{}: cannot open", path.string());
            return false;
        }
        std::stringstream buffer;
        buffer << in.rdbuf();
        std::string text = buffer.str();
        if (text.starts_with("\xEF\xBB\xBF")) {
            text.erase(0, 3);
        }

        std::vector<std::vector<std::string>> records;
        std::vector<int> lines;
        std::vector<std::string> record;
        std::string current;
        bool quoted = false;
        bool field_started = false;
        int line = 1;
        int record_line = 1;
        auto end_field = [&] {
            record.push_back(std::move(current));
            current.clear();
            field_started = false;
        };
        auto end_record = [&] {
            end_field();
            if (!(record.size() == 1 && record[0].empty())) {   // blank lines
                records.push_back(std::move(record));
                lines.push_back(record_line);
            }
            record.clear();
        };
        for (std::size_t i = 0; i < text.size(); ++i) {
            const char c = text[i];
            if (quoted) {
                if (c == '"') {
                    if (i + 1 < text.size() && text[i + 1] == '"') {
                        current += '"';
                        ++i;
                    } else {
                        quoted = false;
                    }
                } else {
                    if (c == '\n') {
                        ++line;
                    }
                    current += c;
                }
            } else if (c == '"' && !field_started) {
                quoted = true;
                field_started = true;
            } else if (c == ',') {
                end_field();
            } else if (c == '\r') {
            } else if (c == '\n') {
                end_record();
                ++line;
                record_line = line;
            } else {
                current += c;
                field_started = true;
            }
        }
        if (quoted) {
            *error = std::format("{}:{}: unterminated quote", path.string(), record_line);
            return false;
        }
        if (!current.empty() || !record.empty()) {
            end_record();
        }
        if (records.empty()) {
            *error = std::format("{}: no header", path.string());
            return false;
        }
        table->header = std::move(records[0]);
        table->rows.assign(records.begin() + 1, records.end());
        table->lines.assign(lines.begin() + 1, lines.end());
        for (std::size_t r = 0; r < table->rows.size(); ++r) {
            if (table->rows[r].size() > table->header.size()) {
                *error = std::format("{}:{}: {} fields, the header has {}", path.string(), table->lines[r],
                                     table->rows[r].size(), table->header.size());
                return false;
            }
            table->rows[r].resize(table->header.size());
        }
        return true;
    }

    std::string field(std::string_view text)
    {
        if (text.find_first_of(",\"\r\n") == std::string_view::npos) {
            return std::string(text);
        }
        std::string out = "\"";
        for (const char c : text) {
            if (c == '"') {
                out += '"';
            }
            out += c;
        }
        out += '"';
        return out;
    }

    bool write_file(const std::filesystem::path& path, const std::string& content, std::string* error)
    {
        std::filesystem::path temp = path;
        temp += ".tmp";
        {
            std::ofstream out(temp, std::ios::binary | std::ios::trunc);
            out.write(content.data(), static_cast<std::streamsize>(content.size()));
            if (!out) {
                *error = std::format("{}: cannot write", temp.string());
                return false;
            }
        }
        std::error_code code;
        std::filesystem::rename(temp, path, code);   // replaces an existing file
        if (code) {
            *error = std::format("{}: cannot replace ({})", path.string(), code.message());
            return false;
        }
        return true;
    }
}
