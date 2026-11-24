#include "core/map_data.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <format>
#include <fstream>

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

        struct Section
        {
            char tag[4];
            std::uint32_t size;
            std::uint64_t offset;
        };
        static_assert(sizeof(Section) == 16);

        template <class T>
        std::span<const T> view(std::span<const std::byte> bytes)
        {
            return { reinterpret_cast<const T*>(bytes.data()), bytes.size() / sizeof(T) };
        }

        // String i of a table written by bake.py `strings`: u32 offsets (one more than strings) + UTF-8 blob.
        std::string_view string_at(std::span<const std::uint32_t> offsets, std::span<const char> blob, std::size_t i)
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
    }

    bool MapData::load(const std::filesystem::path& path, std::string* error)
    {
        auto fail = [&](std::string message) {
            if (error) {
                *error = std::format("{}: {}", path.string(), message);
            }
            return false;
        };

        std::ifstream in(path, std::ios::binary | std::ios::ate);
        if (!in) {
            return fail("cannot open");
        }
        file_.resize(static_cast<std::size_t>(in.tellg()));
        in.seekg(0);
        in.read(reinterpret_cast<char*>(file_.data()), static_cast<std::streamsize>(file_.size()));
        if (!in || file_.size() < sizeof(Header)) {
            return fail("cannot read");
        }

        Header header;
        std::memcpy(&header, file_.data(), sizeof(header));
        if (std::memcmp(header.magic, "MCMP", 4) != 0 || header.version != 2) {
            return fail("not an MCMP v2 file (rebuild with tools/mapbake/bake.py)");
        }
        if (file_.size() < sizeof(Header) + std::size_t{ header.sections } * sizeof(Section)) {
            return fail("truncated section table");
        }
        if (header.width == 0 || header.height == 0 || header.count < 2 || header.count > 0x10000) {
            return fail(std::format("bad header: {}x{} px, {} province slots (rebuild with tools/mapbake/bake.py)",
                                    header.width, header.height, header.count));
        }
        width = header.width;
        height = header.height;
        count = header.count;

        // Every check below records only the first problem; the load fails with it at the end. The file is ours
        // (bake.py), so a problem means a stale or broken bake: the message names the section and asks for a rebuild.
        std::string problem;
        auto bad = [&](std::string message) {
            if (problem.empty()) {
                problem = std::move(message);
            }
        };
        // The table's entries must lie inside the file, 16-aligned (the spans below are cast in place).
        for (std::uint32_t i = 0; i < header.sections; ++i) {
            Section s;
            std::memcpy(&s, file_.data() + sizeof(Header) + i * sizeof(Section), sizeof(s));
            if (s.offset % 16 != 0 || s.offset > file_.size() || s.size > file_.size() - s.offset) {
                bad(std::format("section {} lies outside the file or is misaligned", std::string_view(s.tag, 4)));
            }
        }
        if (!problem.empty()) {
            return fail(std::format("malformed map: {} (rebuild with tools/mapbake/bake.py)", problem));
        }
        auto section = [&](const char* tag) -> std::span<const std::byte> {
            for (std::uint32_t i = 0; i < header.sections; ++i) {
                Section s;
                std::memcpy(&s, file_.data() + sizeof(Header) + i * sizeof(Section), sizeof(s));
                if (std::memcmp(s.tag, tag, 4) == 0) {
                    return { file_.data() + s.offset, s.size };
                }
            }
            return {};
        };
        // A section of exactly `bytes` bytes.
        auto exact = [&](const char* tag, std::size_t bytes) {
            const auto s = section(tag);
            if (s.size() != bytes) {
                bad(std::format("section {} has {} bytes, expected {}", tag, s.size(), bytes));
            }
            return s;
        };
        // A section of whole elements of `unit` bytes (it may be empty only where `may_be_empty`).
        auto whole = [&](const char* tag, std::size_t unit, bool may_be_empty) {
            const auto s = section(tag);
            if (s.size() % unit != 0 || (s.empty() && !may_be_empty)) {
                bad(std::format("section {} has {} bytes, not a{} multiple of {}", tag, s.size(),
                                may_be_empty ? "" : " non-empty", unit));
            }
            return s;
        };
        // A string table of `strings` strings (bake.py `strings`): offsets rising from 0, the last within the blob.
        auto table = [&](const char* tag, std::size_t strings, std::span<const std::uint32_t>* offsets,
                         std::span<const char>* blob) {
            const auto s = section(tag);
            const std::size_t head = (strings + 1) * sizeof(std::uint32_t);
            if (s.size() < head) {
                bad(std::format("section {} has {} bytes, too few for {} strings", tag, s.size(), strings));
                return;
            }
            const auto o = view<std::uint32_t>(s.first(head));
            const auto b = view<char>(s.subspan(head));
            if (o[0] != 0 || !std::ranges::is_sorted(o) || o.back() > b.size()) {
                bad(std::format("section {}: string offsets out of order or past the end", tag));
                return;
            }
            *offsets = o;
            *blob = b;
        };

        const std::size_t n = count;
        const std::size_t pixels = std::size_t{ width } * height;

        // Provinces.
        pixel_province = view<std::uint16_t>(exact("PIDM", pixels * 2));
        pixel_class = view<std::uint8_t>(exact("CLAS", pixels));
        kind = view<ProvinceKind>(exact("PTYP", n));
        region = view<std::uint16_t>(exact("PREG", n * 2));
        color = view<std::uint32_t>(exact("PCOL", n * 4));
        positions = view<float>(exact("PPOS", n * 8 * 4));
        sites = view<std::uint16_t>(exact("PSIT", n * 6 * 2));
        adjacency_offsets = view<std::uint32_t>(exact("ADJO", (n + 1) * 4));
        adjacency_ids = view<std::uint16_t>(whole("ADJI", 2, false));
        table("PNAM", n, &name_offsets_, &name_blob_);

        // Regions and lands: RPAR has a byte per region and [0]; the lands are what RPAR points to.
        region_parent = view<std::uint8_t>(whole("RPAR", 1, false));
        region_count_ = static_cast<std::uint32_t>(region_parent.size());
        land_count_ = 0;
        for (const std::uint8_t l : region_parent) {
            land_count_ = std::max<std::uint32_t>(land_count_, l + 1u);
        }
        table("RNAM", region_count_, &region_name_offsets_, &region_name_blob_);
        table("LNAM", land_count_, &land_name_offsets_, &land_name_blob_);

        // Blocks.
        const auto block_dims = view<std::uint32_t>(exact("BDIM", 16));
        if (block_dims.size() == 4) {
            block_width = block_dims[0];
            block_height = block_dims[1];
            block_size = block_dims[2];
            biome_count_ = block_dims[3];
        }
        if (block_size == 0 || std::size_t{ block_width } * block_size != width ||
            std::size_t{ block_height } * block_size != height || biome_count_ == 0) {
            bad(std::format("section BDIM: {}x{} blocks of {} px do not cover the {}x{} px map, or no biomes",
                            block_width, block_height, block_size, width, height));
        }
        const std::size_t blocks = std::size_t{ block_width } * block_height;
        block_class = view<BlockClass>(exact("BCLS", blocks));
        block_ground = view<std::uint8_t>(exact("BHGT", blocks));
        block_water = view<std::uint8_t>(exact("BWTR", blocks));
        block_biome = view<std::uint8_t>(exact("BBIO", blocks));
        table("BKEY", biome_count_, &biome_key_offsets_, &biome_key_blob_);

        // Borders and rivers (rivers may be empty: a map without rivers.png).
        border_points = view<float>(whole("BVTX", 8, false));
        border_chains = view<std::uint32_t>(whole("BSEG", 16, false));
        river_points = view<float>(whole("RVTX", 12, true));
        river_chains = view<std::uint32_t>(whole("RSEG", 16, true));

        // The names' font and the baked province names.
        const auto font_params = view<std::int32_t>(exact("FPAR", 32));
        if (font_params.size() == 8) {
            font_line_height = font_params[0];
            font_base = font_params[1];
            font_padding = font_params[2];
            font_atlas_scale = font_params[3];
            font_extra = font_params[4];
            font_atlas_width = font_params[5];
            font_atlas_height = font_params[6];
            font_spread = font_params[7];
        }
        if (font_line_height <= 0 || font_atlas_width <= 0 || font_atlas_height <= 0) {
            bad("section FPAR: no line height or atlas size");
        }
        font_chars = view<FontChar>(whole("FCHR", sizeof(FontChar), false));
        font_kerning = view<FontKerning>(whole("FKRN", sizeof(FontKerning), true));
        font_atlas = view<std::uint8_t>(
            exact("FATL", std::size_t(std::max(font_atlas_width, 0)) * std::size_t(std::max(font_atlas_height, 0))));
        province_glyphs = view<float>(whole("NGLY", 7 * 4, true));
        province_glyph_owner = view<std::uint16_t>(exact("NGLP", province_glyphs.size() / 7 * 2));
        if (!problem.empty()) {
            return fail(std::format("malformed map: {} (rebuild with tools/mapbake/bake.py)", problem));
        }

        // Values that index other arrays: ids within their tables, so nothing downstream reads out of bounds.
        auto below = [](auto values, std::size_t limit) {
            return std::ranges::all_of(values, [limit](auto v) { return std::size_t(v) < limit; });
        };
        if (!below(pixel_province, n)) {
            bad("section PIDM: a pixel's province is past the province count");
        }
        if (!below(pixel_class, 4)) {
            bad("section CLAS: unknown pixel class");
        }
        if (!below(kind, 5)) {
            bad("section PTYP: unknown province kind");
        }
        if (!below(region, region_count_)) {
            bad("section PREG: a province's region is past the regions of RPAR");
        }
        if (adjacency_offsets[0] != 0 || !std::ranges::is_sorted(adjacency_offsets) ||
            adjacency_offsets.back() != adjacency_ids.size()) {
            bad("section ADJO: neighbour offsets out of order or not ending at ADJI's size");
        }
        if (!std::ranges::all_of(adjacency_ids, [n](std::uint16_t id) { return id != 0 && id < n; })) {
            bad("section ADJI: a neighbour id is 0 or past the province count");
        }
        for (std::size_t id = 0; id < n; ++id) {
            const std::uint16_t* s = &sites[id * 6];
            const bool house = s[0] == kNoSite ||
                               (s[0] + 1u < block_width && s[1] + 1u < block_height && s[2] < 4);
            const bool pier = s[3] == kNoSite || (s[3] < block_width && s[4] < block_height && s[5] < 4);
            if (!house || !pier) {
                bad(std::format("section PSIT: province {} has its house or pier off the block grid", id));
                break;
            }
        }
        if (!below(block_class, 5)) {
            bad("section BCLS: unknown block class");
        }
        if (!below(block_biome, biome_count_)) {
            bad("section BBIO: a block's biome is past the keys of BKEY");
        }
        const std::size_t border_point_count = border_points.size() / 2;
        for (std::size_t c = 0; c + 3 < border_chains.size(); c += 4) {
            const std::uint32_t pair = border_chains[c + 2];
            if (std::size_t{ border_chains[c] } + border_chains[c + 1] > border_point_count || (pair & 0xFFFF) >= n ||
                (pair >> 16) >= n) {
                bad(std::format("section BSEG: chain {} reaches past BVTX or names a province past the count", c / 4));
                break;
            }
        }
        const std::size_t river_point_count = river_points.size() / 3;
        const std::size_t river_count = river_chains.size() / 4;
        for (std::size_t r = 0; r < river_count; ++r) {
            const std::uint32_t* chain = &river_chains[r * 4];
            if (std::size_t{ chain[0] } + chain[1] > river_point_count ||
                (chain[3] != 0xFFFFFFFFu && chain[3] >= river_count)) {
                bad(std::format("section RSEG: river {} reaches past RVTX or flows into no river", r));
                break;
            }
        }
        auto cp1252 = [](std::int32_t code) { return code >= 0 && code < 256; };
        if (!std::ranges::all_of(font_chars, [&](const FontChar& c) { return cp1252(c.code); }) ||
            !std::ranges::all_of(font_kerning, [&](const FontKerning& k) { return cp1252(k.first) && cp1252(k.second); })) {
            bad("section FCHR or FKRN: a character code outside cp1252");
        }
        for (std::size_t g = 0; g < province_glyph_owner.size(); ++g) {
            const float code = province_glyphs[g * 7 + 6];
            if (province_glyph_owner[g] >= n || !(code >= 0.0f && code < 256.0f) || code != std::floor(code)) {
                bad(std::format("section NGLY or NGLP: glyph {} names a province past the count or is no cp1252 code", g));
                break;
            }
        }
        if (!problem.empty()) {
            return fail(std::format("malformed map: {} (rebuild with tools/mapbake/bake.py)", problem));
        }
        return true;
    }

    std::string_view MapData::region_name(std::uint32_t index) const
    {
        return string_at(region_name_offsets_, region_name_blob_, index);
    }

    std::string_view MapData::land_name(std::uint32_t index) const
    {
        return string_at(land_name_offsets_, land_name_blob_, index);
    }

    std::string_view MapData::biome_key(std::uint32_t index) const
    {
        return string_at(biome_key_offsets_, biome_key_blob_, index);
    }

    std::string_view MapData::province_name(std::uint32_t id) const
    {
        return id < count ? string_at(name_offsets_, name_blob_, id) : std::string_view{};
    }
}
