#include "game/game_data.h"

#include <charconv>
#include <format>

#include "core/csv.h"
#include "core/map_data.h"

namespace mc
{
    namespace
    {
        constexpr std::size_t kMaxListSize = 256;        // u8 indices, 0 = none
        constexpr std::size_t kMaxCountries = 65535;     // u16 indices, 0 = none

        struct Context
        {
            std::filesystem::path path;
            const csv::Table* table = nullptr;
            std::string* error = nullptr;

            bool fail(std::size_t row, const std::string& message) const
            {
                *error = std::format("{}:{}: {}", path.string(), table->lines[row], message);
                return false;
            }
        };

        bool parse_color(std::string_view text, std::uint32_t* out)
        {
            if (text.size() != 7 || text[0] != '#') {
                return false;
            }
            std::uint32_t rgb = 0;
            const auto [ptr, ec] = std::from_chars(text.data() + 1, text.data() + 7, rgb, 16);
            if (ec != std::errc{} || ptr != text.data() + 7) {
                return false;
            }
            *out = ((rgb >> 16) & 0xFF) | (rgb & 0xFF00) | ((rgb & 0xFF) << 16) | 0xFF000000u;
            return true;
        }

        bool valid_key(std::string_view k)
        {
            if (k.empty()) {
                return false;
            }
            for (const char c : k) {
                if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_')) {
                    return false;
                }
            }
            return true;
        }

        // key,name[,color]: `colored` = the colour column is required.
        bool read_list(const std::filesystem::path& path, bool required, bool colored, DefinitionList* list,
                       std::string* error)
        {
            list->key.assign(1, "");
            list->name.assign(1, "");
            list->color.assign(1, 0);
            if (!required && !std::filesystem::exists(path)) {
                return true;
            }
            csv::Table table;
            if (!csv::read(path, &table, error)) {
                return false;
            }
            const Context ctx{ path, &table, error };
            const int key = table.column("key");
            const int name = table.column("name");
            const int color = table.column("color");
            if (key < 0 || name < 0 || (colored && color < 0)) {
                *error = std::format("{}: needs columns key, name{}", path.string(), colored ? ", color" : "");
                return false;
            }
            for (std::size_t r = 0; r < table.rows.size(); ++r) {
                const auto& row = table.rows[r];
                if (!valid_key(row[key])) {
                    return ctx.fail(r, std::format("key \"{}\": lower-case latin, digits, _", row[key]));
                }
                if (list->find(row[key]) >= 0) {
                    return ctx.fail(r, std::format("key \"{}\" repeated", row[key]));
                }
                if (list->size() >= kMaxListSize) {
                    return ctx.fail(r, std::format("more than {} entries", kMaxListSize - 1));
                }
                std::uint32_t c = 0xFF808080u;
                if (colored && !parse_color(row[color], &c)) {
                    return ctx.fail(r, std::format("colour \"{}\": #RRGGBB", row[color]));
                }
                list->key.push_back(row[key]);
                list->name.push_back(row[name].empty() ? row[key] : row[name]);
                list->color.push_back(c);
            }
            return true;
        }

        std::uint32_t pack_tag(std::string_view t)
        {
            return std::uint32_t(std::uint8_t(t[0])) | std::uint32_t(std::uint8_t(t[1])) << 8 |
                   std::uint32_t(std::uint8_t(t[2])) << 16;
        }

        bool valid_tag(std::string_view t)
        {
            if (t.size() != 3) {
                return false;
            }
            for (const char c : t) {
                if (!((c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9'))) {
                    return false;
                }
            }
            return true;
        }

        bool parse_uint(std::string_view text, std::uint32_t* out)
        {
            const auto [ptr, ec] = std::from_chars(text.data(), text.data() + text.size(), *out);
            return ec == std::errc{} && ptr == text.data() + text.size();
        }

        template <class T>
        void fill(Column<T>* column, std::size_t n)
        {
            column->assign(n, T{});
        }
    }

    int DefinitionList::find(std::string_view k) const
    {
        if (k.empty()) {
            return 0;
        }
        for (std::size_t i = 1; i < key.size(); ++i) {
            if (key[i] == k) {
                return static_cast<int>(i);
            }
        }
        return -1;
    }

    int CountryDefinitions::find(std::string_view tag_text) const
    {
        if (tag_text.empty()) {
            return 0;
        }
        if (tag_text.size() != 3) {
            return -1;
        }
        const std::uint32_t packed = pack_tag(tag_text);
        for (std::size_t i = 1; i < tag.size(); ++i) {
            if (tag[i] == packed) {
                return static_cast<int>(i);
            }
        }
        return -1;
    }

    std::string CountryDefinitions::tag_text(std::size_t country) const
    {
        if (country == 0 || country >= tag.size()) {
            return {};
        }
        const std::uint32_t t = tag[country];
        return { char(t & 0xFF), char((t >> 8) & 0xFF), char((t >> 16) & 0xFF) };
    }

    void Provinces::resize(std::uint32_t province_count, std::size_t country_count)
    {
        count = province_count;
        const std::size_t n = padded(province_count);
        fill(&owner, n);
        fill(&controller, n);
        fill(&terrain, n);
        fill(&resource, n);
        core_words = static_cast<std::uint32_t>((country_count + 63) / 64);
        if (core_words == 0) {
            core_words = 1;
        }
        cores.assign(n * core_words, 0);
    }

    void CountryStates::resize(std::size_t country_count)
    {
        count = country_count;
        fill(&capital, padded(count));
    }

    bool GameData::load(const std::filesystem::path& dir, const MapData& map, std::string* error)
    {
        loaded = false;
        const auto common = dir / "common";
        const auto setup = dir / "setup";
        if (!read_list(common / "terrain.csv", true, true, &content.terrain, error) ||
            !read_list(common / "goods.csv", false, true, &content.goods, error)) {
            return false;
        }

        // Countries.
        content.countries = {};
        content.countries.tag.assign(1, 0);
        content.countries.name.assign(1, "");
        content.countries.adjective.assign(1, "");
        content.countries.color.assign(1, 0);
        {
            const auto path = common / "countries.csv";
            csv::Table table;
            if (!csv::read(path, &table, error)) {
                return false;
            }
            const Context ctx{ path, &table, error };
            const int tag = table.column("tag");
            const int name = table.column("name");
            const int color = table.column("color");
            const int adjective = table.column("adjective");   // optional: the name by default
            if (tag < 0 || name < 0 || color < 0) {
                *error = std::format("{}: needs columns tag, name, color (adjective optional)", path.string());
                return false;
            }
            for (std::size_t r = 0; r < table.rows.size(); ++r) {
                const auto& row = table.rows[r];
                if (!valid_tag(row[tag])) {
                    return ctx.fail(r, std::format("tag \"{}\": three capital letters or digits", row[tag]));
                }
                if (content.countries.find(row[tag]) >= 0) {
                    return ctx.fail(r, std::format("tag {} repeated", row[tag]));
                }
                if (content.countries.size() >= kMaxCountries) {
                    return ctx.fail(r, "too many countries");
                }
                std::uint32_t c = 0;
                if (!parse_color(row[color], &c)) {
                    return ctx.fail(r, std::format("colour \"{}\": #RRGGBB", row[color]));
                }
                content.countries.tag.push_back(pack_tag(row[tag]));
                content.countries.name.push_back(row[name].empty() ? row[tag] : row[name]);
                content.countries.adjective.push_back(adjective >= 0 && !row[adjective].empty() ? row[adjective]
                                                                                       : content.countries.name.back());
                content.countries.color.push_back(c);
            }
        }
        state.countries.resize(content.countries.size());
        state.provinces.resize(map.count, content.countries.size());

        auto country_of = [&](const Context& ctx, std::size_t r, std::string_view text, std::uint16_t* out) {
            const int c = content.countries.find(text);
            if (c < 0) {
                return ctx.fail(r, std::format("unknown country \"{}\"", text));
            }
            *out = static_cast<std::uint16_t>(c);
            return true;
        };
        auto land = [&](std::uint32_t id) { return id > 0 && id < map.count && map.kind[id] == ProvinceKind::land; };

        // Province setup.
        {
            const auto path = setup / "provinces.csv";
            csv::Table table;
            if (!csv::read(path, &table, error)) {
                return false;
            }
            const Context ctx{ path, &table, error };
            const char* names[] = { "id", "owner", "controller", "cores", "terrain", "resource" };
            int col[std::size(names)];
            for (std::size_t i = 0; i < std::size(names); ++i) {
                col[i] = table.column(names[i]);
                if (col[i] < 0) {
                    *error = std::format("{}: no column {}", path.string(), names[i]);
                    return false;
                }
            }
            std::vector<std::uint8_t> seen(map.count, 0);
            for (std::size_t r = 0; r < table.rows.size(); ++r) {
                const auto& row = table.rows[r];
                std::uint32_t id = 0;
                if (!parse_uint(row[col[0]], &id) || !land(id)) {
                    return ctx.fail(r, std::format("id \"{}\" is not a land province of the map", row[col[0]]));
                }
                if (seen[id]++) {
                    return ctx.fail(r, std::format("province {} repeated", id));
                }
                if (!country_of(ctx, r, row[col[1]], &state.provinces.owner[id]) ||
                    !country_of(ctx, r, row[col[2]], &state.provinces.controller[id])) {
                    return false;
                }
                std::string_view cores_text = row[col[3]];
                while (!cores_text.empty()) {
                    const std::size_t space = cores_text.find(' ');
                    const std::string_view t = cores_text.substr(0, space);
                    if (!t.empty()) {
                        std::uint16_t c = 0;
                        if (!country_of(ctx, r, t, &c)) {
                            return false;
                        }
                        state.provinces.set_core(id, c, true);
                    }
                    cores_text = space == std::string_view::npos ? std::string_view{} : cores_text.substr(space + 1);
                }
                const struct
                {
                    int column;
                    const DefinitionList* list;
                    Column<std::uint8_t>* out;
                    const char* what;
                } keyed[] = {
                    { col[4], &content.terrain, &state.provinces.terrain, "terrain" },
                    { col[5], &content.goods, &state.provinces.resource, "resource" },
                };
                for (const auto& k : keyed) {
                    const int index = k.list->find(row[k.column]);
                    if (index < 0) {
                        return ctx.fail(r, std::format("unknown {} \"{}\"", k.what, row[k.column]));
                    }
                    (*k.out)[id] = static_cast<std::uint8_t>(index);
                }
            }
        }

        // Country setup (optional: no file = no capitals).
        {
            const auto path = setup / "countries.csv";
            if (std::filesystem::exists(path)) {
                csv::Table table;
                if (!csv::read(path, &table, error)) {
                    return false;
                }
                const Context ctx{ path, &table, error };
                const int tag = table.column("tag");
                const int capital = table.column("capital");
                if (tag < 0 || capital < 0) {
                    *error = std::format("{}: needs columns tag, capital", path.string());
                    return false;
                }
                for (std::size_t r = 0; r < table.rows.size(); ++r) {
                    const auto& row = table.rows[r];
                    const int c = content.countries.find(row[tag]);
                    if (c <= 0) {
                        return ctx.fail(r, std::format("unknown country \"{}\"", row[tag]));
                    }
                    std::uint32_t id = 0;
                    if (!row[capital].empty() && (!parse_uint(row[capital], &id) || !land(id))) {
                        return ctx.fail(r, std::format("capital \"{}\" is not a land province", row[capital]));
                    }
                    state.countries.capital[c] = static_cast<std::uint16_t>(id);
                }
            }
        }
        warnings = check(map);
        loaded = true;
        return true;
    }

    std::string GameData::summary() const
    {
        return std::format("{} countries, {} terrain, {} goods", content.countries.size() - 1,
                           content.terrain.size() - 1, content.goods.size() - 1);
    }

    std::vector<std::string> GameData::check(const MapData& map) const
    {
        std::vector<std::string> out;
        for (std::size_t c = 1; c < content.countries.size(); ++c) {
            const std::uint16_t capital = state.countries.capital[c];
            if (capital != 0 && state.provinces.owner[capital] != c) {
                out.push_back(std::format("{}: capital {} ({}) is not its own", content.countries.tag_text(c), capital,
                                          map.province_name(capital)));
            }
        }
        return out;
    }

    bool GameData::save_setup(const std::filesystem::path& dir, const MapData& map, std::string* error) const
    {
        const Provinces& p = state.provinces;
        const CountryDefinitions& countries = content.countries;
        std::string text = "id,name,owner,controller,cores,terrain,resource\n";
        for (std::uint32_t id = 1; id < map.count; ++id) {
            if (map.kind[id] != ProvinceKind::land) {
                continue;
            }
            std::string cores_text;
            for (std::size_t c = 1; c < countries.size(); ++c) {
                if (p.has_core(id, c)) {
                    if (!cores_text.empty()) {
                        cores_text += ' ';
                    }
                    cores_text += countries.tag_text(c);
                }
            }
            text += std::format("{},{},{},{},{},{},{}\n", id, csv::field(map.province_name(id)),
                                countries.tag_text(p.owner[id]), countries.tag_text(p.controller[id]), cores_text,
                                content.terrain.key[p.terrain[id]], content.goods.key[p.resource[id]]);
        }
        if (!csv::write_file(dir / "setup" / "provinces.csv", text, error)) {
            return false;
        }
        std::string countries_text = "tag,capital\n";
        for (std::size_t c = 1; c < countries.size(); ++c) {
            const std::uint16_t capital = state.countries.capital[c];
            countries_text += std::format("{},{}\n", countries.tag_text(c),
                                          capital ? std::format("{}", capital) : "");
        }
        return csv::write_file(dir / "setup" / "countries.csv", countries_text, error);
    }
}
