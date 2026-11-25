#include "game/game_data.h"

#include <algorithm>
#include <charconv>
#include <cmath>
#include <format>
#include <map>
#include <type_traits>

#include "core/csv.h"
#include "core/log.h"
#include "core/map_data.h"
#include "game/armies.h"

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

        // The data files, read with the patches of a scenario (csv::Patch) applied to the files they name.
        class Source
        {
        public:
            Source(const std::filesystem::path& dir, std::span<const csv::Patch> patches)
                : dir_(dir), patches_(patches), used_(patches.size(), 0)
            {
            }

            bool read(const std::filesystem::path& path, csv::Table* table, std::string* error)
            {
                if (!csv::read(path, table, error)) {
                    return false;
                }
                const std::string file = path.lexically_relative(dir_).generic_string();
                for (std::size_t i = 0; i < patches_.size(); ++i) {
                    const csv::Patch& p = patches_[i];
                    if (p.file != file) {
                        continue;
                    }
                    std::string was;
                    if (!csv::apply(p, table, &was, error)) {
                        return false;
                    }
                    used_[i] = 1;
                    log::info("{}: {}: {}.{} = {} (was {})", p.origin, p.file, p.row, p.column, p.value, was);
                }
                return true;
            }

            // Every patch named a file the load read.
            bool all_used(std::string* error) const
            {
                for (std::size_t i = 0; i < patches_.size(); ++i) {
                    if (!used_[i]) {
                        *error = std::format("{}: {} is not a data file the game reads", patches_[i].origin,
                                             patches_[i].file);
                        return false;
                    }
                }
                return true;
            }

        private:
            std::filesystem::path dir_;
            std::span<const csv::Patch> patches_;
            std::vector<std::uint8_t> used_;
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

        // key,name[,color]: `colored` = the colour column is required. `table`, if given, keeps the file for the
        // list's other columns: row r is entry r + 1 (empty when the optional file is absent).
        bool read_list(Source& src, const std::filesystem::path& path, bool required, bool colored,
                       DefinitionList* list, std::string* error, csv::Table* table_out = nullptr)
        {
            list->key.assign(1, "");
            list->name.assign(1, "");
            list->color.assign(1, 0);
            csv::Table local;
            csv::Table& table = table_out ? *table_out : local;
            table = {};
            if (!required && !std::filesystem::exists(path)) {
                return true;
            }
            if (!src.read(path, &table, error)) {
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

        // A finite number >= 0 (`positive`: > 0); empty is 0 when `empty_zero`.
        bool parse_number(std::string_view text, float* out, bool empty_zero = false, bool positive = false)
        {
            if (text.empty()) {
                *out = 0.0f;
                return empty_zero && !positive;
            }
            const auto [ptr, ec] = std::from_chars(text.data(), text.data() + text.size(), *out);
            return ec == std::errc{} && ptr == text.data() + text.size() && std::isfinite(*out) &&
                   (positive ? *out > 0.0f : *out >= 0.0f);
        }

        // The column, or an error naming the file and the column.
        bool need_column(const std::filesystem::path& path, const csv::Table& table, const char* name, int* out,
                         std::string* error)
        {
            *out = table.column(name);
            if (*out < 0) {
                *error = std::format("{}: no column {}", path.string(), name);
                return false;
            }
            return true;
        }

        template <class T>
        void fill(Column<T>* column, std::size_t n)
        {
            column->assign(n, T{});
        }

        // The words of a column list's `start` (game/game_data.h). They resolve here, inside this namespace, where
        // the lists expand with their start.
        namespace start
        {
            struct Runtime {};
            struct ByHand {};
            struct Derived {};
            struct Country {};
            struct Number {};
            struct Key
            {
                DefinitionList Content::* list;
            };
            template <class Parse>
            struct Setup
            {
                std::string_view csv;
                Parse parse;
            };

            inline constexpr Runtime runtime{};
            inline constexpr ByHand by_hand{};
            inline constexpr Derived derived{};
            inline constexpr Country country{};
            inline constexpr Number number{};
            constexpr Key key(DefinitionList Content::* list) { return { list }; }
            template <class Parse>
            constexpr Setup<Parse> setup(std::string_view csv, Parse parse)
            {
                return { csv, parse };
            }

            template <class T>
            inline constexpr bool kSetup = false;
            template <class Parse>
            inline constexpr bool kSetup<Setup<Parse>> = true;

            // each(table, f): f(start, column) for every column of the table's list, in its order.
#define MC_EACH_START(table, type, name, start) f(start, columns.name);
            template <class T, class F>
                requires std::is_same_v<std::remove_const_t<T>, Provinces>
            void each(T& columns, F&& f)
            {
                MC_PROVINCE_COLUMNS(MC_EACH_START)
            }
            template <class T, class F>
                requires std::is_same_v<std::remove_const_t<T>, Regions>
            void each(T& columns, F&& f)
            {
                MC_REGION_COLUMNS(MC_EACH_START)
            }
            template <class T, class F>
                requires std::is_same_v<std::remove_const_t<T>, CountryStates>
            void each(T& columns, F&& f)
            {
                MC_COUNTRY_COLUMNS(MC_EACH_START)
            }
            template <class T, class F>
                requires std::is_same_v<std::remove_const_t<T>, Armies>
            void each(T& columns, F&& f)
            {
                MC_ARMY_COLUMNS(MC_EACH_START)
            }
            template <class T, class F>
                requires std::is_same_v<std::remove_const_t<T>, Regiments>
            void each(T& columns, F&& f)
            {
                MC_REGIMENT_COLUMNS(MC_EACH_START)
            }
#undef MC_EACH_START

#define MC_RUNTIME_ONLY(table, type, name, start)                                                                     \
    static_assert(std::is_same_v<std::remove_cvref_t<decltype(start)>, Runtime>,                                      \
                  #table "." #name ": balances have no setup file, they start at 0");
            MC_BALANCE_COLUMNS(MC_RUNTIME_ONLY)
#undef MC_RUNTIME_ONLY
        }

        // One field of a setup column (`csv` its name in the file) parsed into `out`. The overload is the parse of the
        // column's start; a column whose type does not fit its parse has none and does not compile.
        bool parse_field(const Context& ctx, std::size_t r, const Content& content, std::string_view, start::Country,
                         std::string_view text, std::uint16_t* out)
        {
            const int c = content.countries.find(text);
            if (c < 0) {
                return ctx.fail(r, std::format("unknown country \"{}\"", text));
            }
            *out = static_cast<std::uint16_t>(c);
            return true;
        }
        bool parse_field(const Context& ctx, std::size_t r, const Content& content, std::string_view csv,
                         start::Key key, std::string_view text, std::uint8_t* out)
        {
            const int index = (content.*key.list).find(text);
            if (index < 0) {
                return ctx.fail(r, std::format("unknown {} \"{}\"", csv, text));
            }
            *out = static_cast<std::uint8_t>(index);
            return true;
        }
        bool parse_field(const Context& ctx, std::size_t r, const Content&, std::string_view csv, start::Number,
                         std::string_view text, float* out)
        {
            if (!parse_number(text, out, true)) {
                return ctx.fail(r, std::format("{} \"{}\": a number >= 0", csv, text));
            }
            return true;
        }

        // The text of one field of a setup column, as the file holds it.
        std::string format_field(const Content& content, start::Country, std::uint16_t value)
        {
            return content.countries.tag_text(value);
        }
        std::string format_field(const Content& content, start::Key key, std::uint8_t value)
        {
            return (content.*key.list).key[value];
        }
        std::string format_field(const Content&, start::Number, float value)
        {
            return std::format("{}", value);   // the shortest text that reads back to the same float
        }

        // One data/setup file through a table's column list. Its columns are the table's by-hand ones (named by the
        // caller: the row's key, the name for people, ...) and the list's setup columns, which are read and written
        // here; the file's order of them is its layout (GameData::setup_layout).
        class SetupFile
        {
        public:
            template <class T>
            SetupFile(const T& columns, std::span<const std::string_view> by_hand) : by_hand_(by_hand)
            {
                start::each(columns, [&](auto s, const auto&) {
                    if constexpr (start::kSetup<decltype(s)>) {
                        setup_.push_back(s.csv);
                    }
                });
            }

            // Finds every setup column in the file's header.
            bool find(const std::filesystem::path& path, const csv::Table& table, std::string* error)
            {
                column_.assign(setup_.size(), -1);
                for (std::size_t k = 0; k < setup_.size(); ++k) {
                    column_[k] = table.column(setup_[k]);
                    if (column_[k] < 0) {
                        *error = std::format("{}: no column {}", path.string(), setup_[k]);
                        return false;
                    }
                }
                return true;
            }

            // Parses row r of the file (found) into entry `index` of the columns.
            template <class T>
            bool read(const Context& ctx, std::size_t r, std::size_t index, const Content& content, T& columns) const
            {
                const auto& row = ctx.table->rows[r];
                std::size_t k = 0;
                bool ok = true;
                start::each(columns, [&](auto s, auto& column) {
                    if constexpr (start::kSetup<decltype(s)>) {
                        ok = ok && parse_field(ctx, r, content, s.csv, s.parse, row[column_[k]], &column[index]);
                        ++k;
                    }
                });
                return ok;
            }

            // The layout: the file's own columns from `order` (a header) in its order, once each, then the ones it
            // lacks: by hand first, then the list's.
            [[nodiscard]] std::vector<std::string> layout(std::span<const std::string> order) const
            {
                std::vector<std::string> out;
                auto add = [&](std::string_view name) {
                    if (std::find(out.begin(), out.end(), name) == out.end()) {
                        out.emplace_back(name);
                    }
                };
                for (const std::string& name : order) {
                    if (known(name)) {
                        add(name);
                    }
                }
                for (const std::string_view name : by_hand_) {
                    add(name);
                }
                for (const std::string_view name : setup_) {
                    add(name);
                }
                return out;
            }

            // The file's text: the header of `layout` (completed by layout()), then a row per entry, where a by-hand
            // column's field is hand(name, entry) and a setup column's is the entry's value.
            template <class T, class Hand>
            [[nodiscard]] std::string write(std::span<const std::string> layout_order, const T& columns,
                                            const Content& content, std::span<const std::uint32_t> entries,
                                            Hand&& hand) const
            {
                const std::vector<std::string> order = layout(layout_order);
                // Each column of the order: the index of its field, by-hand fields first, then the setup ones.
                std::vector<std::size_t> field_of;
                std::string text;
                for (const std::string& name : order) {
                    const auto h = std::find(by_hand_.begin(), by_hand_.end(), name);
                    field_of.push_back(h != by_hand_.end()
                                           ? std::size_t(h - by_hand_.begin())
                                           : by_hand_.size() + std::size_t(std::find(setup_.begin(), setup_.end(),
                                                                                     name) - setup_.begin()));
                    text += text.empty() ? name : "," + name;
                }
                text += '\n';
                std::vector<std::string> fields(by_hand_.size() + setup_.size());
                for (const std::uint32_t entry : entries) {
                    for (std::size_t k = 0; k < by_hand_.size(); ++k) {
                        fields[k] = hand(by_hand_[k], entry);
                    }
                    std::size_t k = by_hand_.size();
                    start::each(columns, [&](auto s, const auto& column) {
                        if constexpr (start::kSetup<decltype(s)>) {
                            fields[k++] = format_field(content, s.parse, column[entry]);
                        }
                    });
                    for (std::size_t i = 0; i < field_of.size(); ++i) {
                        if (i > 0) {
                            text += ',';
                        }
                        text += csv::field(fields[field_of[i]]);
                    }
                    text += '\n';
                }
                return text;
            }

        private:
            [[nodiscard]] bool known(std::string_view name) const
            {
                return std::find(by_hand_.begin(), by_hand_.end(), name) != by_hand_.end() ||
                       std::find(setup_.begin(), setup_.end(), name) != setup_.end();
            }

            std::span<const std::string_view> by_hand_;
            std::vector<std::string_view> setup_;   // the list's setup columns, in list order
            std::vector<int> column_;               // where each is in the file being read
        };

        // The by-hand columns of each data/setup file (SetupFile).
        constexpr std::string_view kProvinceHand[] = { "id", "name", "cores" };
        constexpr std::string_view kRegionHand[] = { "region", "name" };
        constexpr std::string_view kCountryHand[] = { "tag", "capital" };
        constexpr std::string_view kArmyHand[] = { "army", "province" };
        constexpr std::string_view kRegimentHand[] = { "army" };

        // goods.csv (required: every land province has a good): key, name, color, price (the base price, emeralds per
        // unit produced, > 0; the money good is worth 1 whatever it says).
        bool read_goods(Source& src, const std::filesystem::path& path, DefinitionList* goods,
                        std::vector<float>* price, std::string* error)
        {
            csv::Table table;
            if (!read_list(src, path, true, true, goods, error, &table)) {
                return false;
            }
            price->assign(goods->size(), 0.0f);
            const Context ctx{ path, &table, error };
            int column = 0;
            if (!need_column(path, table, "price", &column, error)) {
                return false;
            }
            for (std::size_t r = 0; r < table.rows.size(); ++r) {
                if (goods->key[r + 1] == kMoneyGood) {
                    (*price)[r + 1] = 1.0f;
                } else if (!parse_number(table.rows[r][column], &(*price)[r + 1], false, true)) {
                    return ctx.fail(r, std::format("price \"{}\": emeralds per unit, > 0", table.rows[r][column]));
                }
            }
            return true;
        }

        // regiment_types.csv (optional: no file = no regiments): key, name, category (infantry, cavalry, artillery).
        bool read_regiment_types(Source& src, const std::filesystem::path& path, DefinitionList* types,
                                 std::vector<RegimentCategory>* category, std::string* error)
        {
            csv::Table table;
            if (!read_list(src, path, false, false, types, error, &table)) {
                return false;
            }
            category->assign(types->size(), RegimentCategory::infantry);
            if (table.header.empty()) {
                return true;
            }
            const Context ctx{ path, &table, error };
            int column = 0;
            if (!need_column(path, table, "category", &column, error)) {
                return false;
            }
            for (std::size_t r = 0; r < table.rows.size(); ++r) {
                const std::string& text = table.rows[r][column];
                const auto* found = std::find(std::begin(kRegimentCategories), std::end(kRegimentCategories), text);
                if (found == std::end(kRegimentCategories)) {
                    return ctx.fail(r, std::format("category \"{}\": infantry, cavalry or artillery", text));
                }
                (*category)[r + 1] = static_cast<RegimentCategory>(found - std::begin(kRegimentCategories));
            }
            return true;
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

#define MC_FILL_COLUMN(table, type, name, start) fill(&name, n);
    void Provinces::resize(std::uint32_t province_count, std::size_t country_count)
    {
        count = province_count;
        const std::size_t n = padded(province_count);
        MC_PROVINCE_COLUMNS(MC_FILL_COLUMN)
        core_words = static_cast<std::uint32_t>((country_count + 63) / 64);
        if (core_words == 0) {
            core_words = 1;
        }
        cores.assign(n * core_words, 0);
    }

    void Regions::resize(std::uint32_t region_count)
    {
        count = region_count;
        const std::size_t n = padded(region_count);
        MC_REGION_COLUMNS(MC_FILL_COLUMN)
    }

    void CountryStates::resize(std::size_t country_count)
    {
        count = country_count;
        const std::size_t n = padded(count);
        MC_COUNTRY_COLUMNS(MC_FILL_COLUMN)
    }

    void Armies::resize()
    {
        const std::size_t n = kArmySlots;
        MC_ARMY_COLUMNS(MC_FILL_COLUMN)
        alive.assign(kArmySlots / 64, 0);
    }

    void Regiments::resize()
    {
        const std::size_t n = kRegimentSlots;
        MC_REGIMENT_COLUMNS(MC_FILL_COLUMN)
        alive.assign(kRegimentSlots / 64, 0);
    }
#undef MC_FILL_COLUMN

    bool GameData::load(const std::filesystem::path& dir, const MapData& map, std::string* error,
                        std::span<const csv::Patch> patches)
    {
        loaded = false;
        Source src(dir, patches);
        const auto common = dir / "common";
        const auto setup = dir / "setup";
        if (!read_list(src, common / "terrain.csv", true, true, &content.terrain, error) ||
            !read_goods(src, common / "goods.csv", &content.goods, &content.goods_price, error) ||
            !read_list(src, common / "trade_nodes.csv", false, true, &content.trade_nodes, error) ||
            !read_regiment_types(src, common / "regiment_types.csv", &content.regiment_types,
                                 &content.regiment_category, error)) {
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
            if (!src.read(path, &table, error)) {
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
        state.regions.resize(map.region_count());
        content.province_region.assign(map.region.begin(), map.region.end());
        state.money = {};   // runtime: no accounts yet (their resize comes with the treasury, M8)
        state.armies.resize();
        state.regiments.resize();

        auto country_of = [&](const Context& ctx, std::size_t r, std::string_view text, std::uint16_t* out) {
            const int c = content.countries.find(text);
            if (c < 0) {
                return ctx.fail(r, std::format("unknown country \"{}\"", text));
            }
            *out = static_cast<std::uint16_t>(c);
            return true;
        };
        auto land = [&](std::uint32_t id) { return id > 0 && id < map.count && map.kind[id] == ProvinceKind::land; };

        // Province setup: id (a land province of the map) and cores by hand, name only for people; the list's setup
        // columns.
        {
            const auto path = setup / "provinces.csv";
            csv::Table table;
            if (!src.read(path, &table, error)) {
                return false;
            }
            const Context ctx{ path, &table, error };
            SetupFile file(state.provinces, kProvinceHand);
            int id_column = 0;
            int cores_column = 0;
            if (!need_column(path, table, "id", &id_column, error) ||
                !need_column(path, table, "cores", &cores_column, error) || !file.find(path, table, error)) {
                return false;
            }
            setup_layout.provinces = file.layout(table.header);
            std::vector<std::uint8_t> seen(map.count, 0);
            for (std::size_t r = 0; r < table.rows.size(); ++r) {
                const auto& row = table.rows[r];
                std::uint32_t id = 0;
                if (!parse_uint(row[id_column], &id) || !land(id)) {
                    return ctx.fail(r, std::format("id \"{}\" is not a land province of the map", row[id_column]));
                }
                if (seen[id]++) {
                    return ctx.fail(r, std::format("province {} repeated", id));
                }
                if (!file.read(ctx, r, id, content, state.provinces)) {
                    return false;
                }
                std::string_view cores_text = row[cores_column];
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
            }
        }

        // Region setup (optional: no file = every column at 0, no region in a trade node): region (the map's number)
        // by hand, name only for people; the list's setup columns.
        {
            const auto path = setup / "regions.csv";
            SetupFile file(state.regions, kRegionHand);
            setup_layout.regions = file.layout({});
            if (std::filesystem::exists(path)) {
                csv::Table table;
                if (!src.read(path, &table, error)) {
                    return false;
                }
                const Context ctx{ path, &table, error };
                int region = 0;
                if (!need_column(path, table, "region", &region, error) || !file.find(path, table, error)) {
                    return false;
                }
                setup_layout.regions = file.layout(table.header);
                std::vector<std::uint8_t> seen(state.regions.count, 0);
                for (std::size_t r = 0; r < table.rows.size(); ++r) {
                    const auto& row = table.rows[r];
                    std::uint32_t id = 0;
                    if (!parse_uint(row[region], &id) || id == 0 || id >= state.regions.count) {
                        return ctx.fail(r, std::format("region \"{}\": not a region of the map", row[region]));
                    }
                    if (seen[id]++) {
                        return ctx.fail(r, std::format("region {} repeated", id));
                    }
                    if (!file.read(ctx, r, id, content, state.regions)) {
                        return false;
                    }
                }
            }
        }

        // Country setup (optional: no file = every column at 0, no capitals): tag and capital (a land province) by
        // hand; the list's setup columns.
        {
            const auto path = setup / "countries.csv";
            SetupFile file(state.countries, kCountryHand);
            setup_layout.countries = file.layout({});
            if (std::filesystem::exists(path)) {
                csv::Table table;
                if (!src.read(path, &table, error)) {
                    return false;
                }
                const Context ctx{ path, &table, error };
                int tag = 0;
                int capital = 0;
                if (!need_column(path, table, "tag", &tag, error) ||
                    !need_column(path, table, "capital", &capital, error) || !file.find(path, table, error)) {
                    return false;
                }
                setup_layout.countries = file.layout(table.header);
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
                    if (!file.read(ctx, r, std::size_t(c), content, state.countries)) {
                        return false;
                    }
                }
            }
        }

        // Armies at the start (optional, no file = none): army (its number in the file, for regiments.csv) and
        // province (a land province) by hand, the list's setup columns; each takes the next slot in file order.
        std::map<std::uint32_t, std::uint16_t> army_slot;   // number in the file -> slot
        {
            const auto path = setup / "armies.csv";
            SetupFile file(state.armies, kArmyHand);
            setup_layout.armies = file.layout({});
            if (std::filesystem::exists(path)) {
                csv::Table table;
                if (!src.read(path, &table, error)) {
                    return false;
                }
                const Context ctx{ path, &table, error };
                int number = 0;
                int province = 0;
                if (!need_column(path, table, "army", &number, error) ||
                    !need_column(path, table, "province", &province, error) || !file.find(path, table, error)) {
                    return false;
                }
                setup_layout.armies = file.layout(table.header);
                for (std::size_t r = 0; r < table.rows.size(); ++r) {
                    const auto& row = table.rows[r];
                    std::uint32_t n = 0;
                    if (!parse_uint(row[number], &n) || n == 0) {
                        return ctx.fail(r, std::format("army \"{}\": a number from 1", row[number]));
                    }
                    if (army_slot.contains(n)) {
                        return ctx.fail(r, std::format("army {} repeated", n));
                    }
                    std::uint32_t id = 0;
                    if (!parse_uint(row[province], &id) || !land(id)) {
                        return ctx.fail(r, std::format("province \"{}\" is not a land province", row[province]));
                    }
                    std::uint16_t slot = 0;
                    if (!create_army(state.armies, 0, static_cast<std::uint16_t>(id), &slot)) {
                        return ctx.fail(r, std::format("more than {} armies", kArmySlots));
                    }
                    army_slot[n] = slot;
                    if (!file.read(ctx, r, slot, content, state.armies)) {
                        return false;
                    }
                }
            }
        }

        // Their regiments (optional): army (its number in armies.csv) by hand, the list's setup columns; morale full,
        // the army owner's.
        {
            const auto path = setup / "regiments.csv";
            SetupFile file(state.regiments, kRegimentHand);
            setup_layout.regiments = file.layout({});
            if (std::filesystem::exists(path)) {
                csv::Table table;
                if (!src.read(path, &table, error)) {
                    return false;
                }
                const Context ctx{ path, &table, error };
                int number = 0;
                if (!need_column(path, table, "army", &number, error) || !file.find(path, table, error)) {
                    return false;
                }
                setup_layout.regiments = file.layout(table.header);
                for (std::size_t r = 0; r < table.rows.size(); ++r) {
                    const auto& row = table.rows[r];
                    std::uint32_t n = 0;
                    const auto army = parse_uint(row[number], &n) ? army_slot.find(n) : army_slot.end();
                    if (army == army_slot.end()) {
                        return ctx.fail(r, std::format("army \"{}\" is not in armies.csv", row[number]));
                    }
                    const std::uint16_t owner = state.armies.owner[army->second];
                    std::uint16_t slot = 0;
                    if (!add_regiment(state.regiments, army->second, 0, 0.0f, state.countries.morale[owner], &slot)) {
                        return ctx.fail(r, std::format("more than {} regiments", kRegimentSlots));
                    }
                    if (!file.read(ctx, r, slot, content, state.regiments)) {
                        return false;
                    }
                }
            }
        }
        if (!src.all_used(error)) {
            return false;
        }
        warnings = check(map);
        loaded = true;
        return true;
    }

    std::string GameData::summary() const
    {
        std::size_t armies = 0;
        std::size_t regiments = 0;
        for_each_alive(state.armies.alive, [&](std::uint32_t) { ++armies; });
        for_each_alive(state.regiments.alive, [&](std::uint32_t) { ++regiments; });
        return std::format("{} countries, {} terrain, {} goods, {} trade nodes, {} regiment types; {} armies, {} "
                           "regiments",
                           content.countries.size() - 1, content.terrain.size() - 1, content.goods.size() - 1,
                           content.trade_nodes.size() - 1, content.regiment_types.size() - 1, armies, regiments);
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

        // Every land province produces a good. A warning, not a load error: a land province without a row (new on the
        // map) has none until the editor gives it one. The first few by name, then a count.
        constexpr std::size_t kNamed = 10;
        std::size_t without_good = 0;
        for (std::uint32_t id = 1; id < map.count; ++id) {
            if (map.kind[id] == ProvinceKind::land && state.provinces.good[id] == 0) {
                if (++without_good <= kNamed) {
                    out.push_back(std::format("province {} ({}): no good", id, map.province_name(id)));
                }
            }
        }
        if (without_good > kNamed) {
            out.push_back(std::format("... {} land provinces without a good in all", without_good));
        }

        // Armies: an owner, at least one regiment; regiments: a type. Armies are named by their slot here, as the
        // editor and the game name them ("army 3": the 3rd Army).
        const Armies& armies = state.armies;
        const Regiments& regiments = state.regiments;
        std::vector<std::uint32_t> regiments_of(kArmySlots, 0);
        for_each_alive(regiments.alive, [&](std::uint32_t r) {
            ++regiments_of[regiments.army[r]];
            if (regiments.type[r] == 0) {
                out.push_back(std::format("army {} in {}: a regiment without a type", regiments.army[r] + 1,
                                          map.province_name(armies.province[regiments.army[r]])));
            }
        });
        for_each_alive(armies.alive, [&](std::uint32_t a) {
            if (armies.owner[a] == 0) {
                out.push_back(std::format("army {} in {}: no owner", a + 1, map.province_name(armies.province[a])));
            }
            if (regiments_of[a] == 0) {
                out.push_back(std::format("army {} in {}: no regiments", a + 1, map.province_name(armies.province[a])));
            }
        });

        // Trade nodes: every region in one, no node empty (once there are nodes at all).
        if (content.trade_nodes.size() > 1) {
            std::vector<std::size_t> regions_of(content.trade_nodes.size(), 0);
            for (std::uint32_t r = 1; r < state.regions.count; ++r) {
                const std::uint8_t n = state.regions.trade_node[r];
                ++regions_of[n];
                if (n == 0) {
                    out.push_back(std::format("region {} ({}): no trade node", r, map.region_name(r)));
                }
            }
            for (std::size_t n = 1; n < content.trade_nodes.size(); ++n) {
                if (regions_of[n] == 0) {
                    out.push_back(std::format("trade node {}: no regions", content.trade_nodes.key[n]));
                }
            }
        }
        return out;
    }

    bool GameData::save_setup(const std::filesystem::path& dir, const MapData& map, std::string* error) const
    {
        const Provinces& p = state.provinces;
        const CountryDefinitions& countries = content.countries;
        std::vector<std::uint32_t> entries;
        for (std::uint32_t id = 1; id < map.count; ++id) {
            if (map.kind[id] == ProvinceKind::land) {
                entries.push_back(id);
            }
        }
        const std::string provinces_text = SetupFile(p, kProvinceHand).write(
            setup_layout.provinces, p, content, entries, [&](std::string_view column, std::uint32_t id) {
                if (column == "id") {
                    return std::format("{}", id);
                }
                if (column == "name") {
                    return std::string(map.province_name(id));
                }
                std::string cores_text;   // "cores"
                for (std::size_t c = 1; c < countries.size(); ++c) {
                    if (p.has_core(id, c)) {
                        if (!cores_text.empty()) {
                            cores_text += ' ';
                        }
                        cores_text += countries.tag_text(c);
                    }
                }
                return cores_text;
            });
        if (!csv::write_file(dir / "setup" / "provinces.csv", provinces_text, error)) {
            return false;
        }

        entries.clear();
        for (std::uint32_t r = 1; r < state.regions.count; ++r) {
            entries.push_back(r);
        }
        const std::string regions_text = SetupFile(state.regions, kRegionHand).write(
            setup_layout.regions, state.regions, content, entries, [&](std::string_view column, std::uint32_t r) {
                return column == "region" ? std::format("{}", r) : std::string(map.region_name(r));   // "name"
            });
        if (!csv::write_file(dir / "setup" / "regions.csv", regions_text, error)) {
            return false;
        }

        entries.clear();
        for (std::uint32_t c = 1; c < countries.size(); ++c) {
            entries.push_back(c);
        }
        const std::string countries_text = SetupFile(state.countries, kCountryHand).write(
            setup_layout.countries, state.countries, content, entries, [&](std::string_view column, std::uint32_t c) {
                if (column == "tag") {
                    return countries.tag_text(c);
                }
                const std::uint16_t capital = state.countries.capital[c];   // "capital"
                return capital ? std::format("{}", capital) : std::string{};
            });
        if (!csv::write_file(dir / "setup" / "countries.csv", countries_text, error)) {
            return false;
        }

        // The live armies in slot order, numbered 1, 2, ... in it; their regiments in slot order.
        entries.clear();
        std::vector<std::uint32_t> number_of(kArmySlots, 0);
        for_each_alive(state.armies.alive, [&](std::uint32_t a) {
            entries.push_back(a);
            number_of[a] = static_cast<std::uint32_t>(entries.size());
        });
        const std::string armies_text = SetupFile(state.armies, kArmyHand).write(
            setup_layout.armies, state.armies, content, entries, [&](std::string_view column, std::uint32_t a) {
                return std::format("{}", column == "army" ? number_of[a] : state.armies.province[a]);   // "province"
            });
        if (!csv::write_file(dir / "setup" / "armies.csv", armies_text, error)) {
            return false;
        }
        entries.clear();
        for_each_alive(state.regiments.alive, [&](std::uint32_t r) { entries.push_back(r); });
        const std::string regiments_text = SetupFile(state.regiments, kRegimentHand).write(
            setup_layout.regiments, state.regiments, content, entries, [&](std::string_view, std::uint32_t r) {
                return std::format("{}", number_of[state.regiments.army[r]]);   // "army"
            });
        return csv::write_file(dir / "setup" / "regiments.csv", regiments_text, error);
    }
}
