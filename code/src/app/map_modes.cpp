#include "app/map_modes.h"

#include <algorithm>
#include <cmath>

#include "game/game_data.h"
#include "core/map_data.h"

namespace mc
{
    namespace
    {
        ProvinceFill hsv(float h, float s, float v)
        {
            const float k[3] = { 5.0f, 3.0f, 1.0f };
            float rgb[3];
            for (int i = 0; i < 3; ++i) {
                const float t = std::fmod(k[i] + h * 6.0f, 6.0f);
                rgb[i] = v - v * s * std::max(0.0f, std::min({ t, 4.0f - t, 1.0f }));
            }
            return { rgb[0], rgb[1], rgb[2], 1.0f };
        }

        // Distinct, map-like colours for regions: golden-ratio hues, varied saturation and value.
        ProvinceFill region_color(std::uint32_t region)
        {
            const float h = std::fmod(static_cast<float>(region) * 0.618034f + 0.07f, 1.0f);
            const float s = 0.42f + 0.18f * std::fmod(static_cast<float>(region) * 0.37f, 1.0f);
            const float v = 0.70f + 0.20f * std::fmod(static_cast<float>(region) * 0.53f, 1.0f);
            return hsv(h, s, v);
        }

        ProvinceFill rgba(std::uint32_t c)
        {
            return { (c & 255) / 255.0f, ((c >> 8) & 255) / 255.0f, ((c >> 16) & 255) / 255.0f, 1.0f };
        }

        ProvinceFill mix(ProvinceFill a, ProvinceFill b, float t)
        {
            return { a.r + (b.r - a.r) * t, a.g + (b.g - a.g) * t, a.b + (b.b - a.b) * t, 1.0f };
        }

        constexpr ProvinceFill kNone{ 0.55f, 0.55f, 0.55f, 1.0f };   // a field not set yet
        constexpr ProvinceFill kWasteland{ 90.0f / 255.0f, 90.0f / 255.0f, 90.0f / 255.0f, 1.0f };   // all modes but terrain
        constexpr std::int32_t kMinBridgedPiece = 1;   // water joins pieces of one province too (map_modes.h)

        ProvinceFill listed(const DefinitionList& list, std::uint8_t index)
        {
            return index != 0 && index < list.size() ? rgba(list.color[index]) : kNone;
        }

        bool has_game(const GameData* game) { return game != nullptr && game->loaded; }

        constexpr int kRequiredFields = 4;
        int missing_fields(const Provinces& p, std::uint32_t id)
        {
            return (p.owner[id] == 0) + (p.controller[id] == 0) + (p.terrain[id] == 0) + (p.rgo[id] == 0);
        }

        // EU4's impassable provinces: the wastelands, land that no realm takes and names do not cross.
        std::vector<std::uint8_t> impassable(const MapData& map)
        {
            std::vector<std::uint8_t> out(map.count, 0);
            for (std::uint32_t id = 1; id < map.count; ++id) {
                out[id] = map.kind[id] == ProvinceKind::wasteland;
            }
            return out;
        }

        // Names of province groups without capitals (regions): each key a "country" of EU4's
        // realm names, so every mode joins its pieces across straits and lakes the same way as the countries.
        std::vector<MapLabel> group_names(const MapData& map, const LabelRaster& raster, const LabelFont& font,
                                          std::vector<std::int32_t> owner, std::vector<std::string> names)
        {
            RealmNames realms;
            realms.impassable = impassable(map);
            realms.owner = std::move(owner);
            realms.owner.resize(map.count, -1);
            realms.tribal_owner.assign(map.count, -1);
            realms.capital.assign(names.size(), 0);
            realms.min_bridged_piece = kMinBridgedPiece;
            realms.name = std::move(names);
            realms.adjective = realms.name;
            return generate_names(raster, font, realm_records(raster, realms));
        }

        std::vector<MapLabel> region_names(const MapData& map, const LabelRaster& raster, const LabelFont& font)
        {
            const std::uint32_t regions = map.region_count();
            std::vector<std::int32_t> owner(map.count, -1);
            for (std::uint32_t id = 1; id < map.count; ++id) {
                const std::uint16_t r = map.region[id];
                if (map.kind[id] == ProvinceKind::land && r != 0 && r < regions) {
                    owner[id] = r;
                }
            }
            std::vector<std::string> names;
            for (std::uint32_t r = 0; r < regions; ++r) {
                names.emplace_back(map.region_name(r));
            }
            return group_names(map, raster, font, std::move(owner), std::move(names));
        }

        std::vector<MapLabel> country_names(const MapData& map, const GameData& game, const LabelRaster& raster,
                                            const LabelFont& font)
        {
            // EU4's geography for the names of parts cut off from the capital: its regions are large (Western
            // Europe), its areas small. Ours: the lands (the regions' parents) are the regions, the map's regions the
            // areas. A part in the capital's land keeps the plain name; one elsewhere is "$ADJECTIVE$ $LAND$", a
            // small one "$ADJECTIVE$ $REGION$".
            const CountryDefinitions& countries = game.content.countries;
            const Provinces& provinces = game.state.provinces;
            RealmNames realms;
            realms.impassable = impassable(map);
            realms.owner.assign(map.count, -1);
            realms.tribal_owner.assign(map.count, -1);
            realms.region.assign(map.count, -1);
            realms.area.assign(map.count, -1);
            for (std::uint32_t id = 1; id < map.count; ++id) {
                if (map.kind[id] != ProvinceKind::land) {
                    continue;
                }
                if (provinces.owner[id] != 0) {
                    realms.owner[id] = provinces.owner[id];
                }
                const std::uint16_t r = map.region[id];
                if (r != 0 && r < map.region_parent.size() && map.region_parent[r] != 0) {
                    realms.region[id] = map.region_parent[r];
                    realms.area[id] = r;
                }
            }
            const Column<std::uint16_t>& capital = game.state.countries.capital;
            realms.capital.assign(capital.begin(), capital.begin() + countries.size());
            realms.min_bridged_piece = kMinBridgedPiece;
            realms.name = countries.name;
            realms.adjective = countries.adjective;
            for (std::uint32_t l = 0; l < map.land_count(); ++l) {
                realms.region_names.emplace_back(map.land_name(l));
            }
            for (std::uint32_t r = 0; r < map.region_count(); ++r) {
                realms.area_names.emplace_back(map.region_name(r));
            }
            return generate_names(raster, font, realm_records(raster, realms));
        }
    }

    const char* map_mode_name(MapMode mode)
    {
        switch (mode) {
        case MapMode::terrain: return "Terrain (look)";
        case MapMode::political: return "Political";
        case MapMode::provinces: return "Provinces";
        case MapMode::regions: return "Regions";
        case MapMode::terrain_type: return "Terrain (logical)";
        case MapMode::rgo: return "RGO";
        case MapMode::unfilled: return "Unfilled";
        }
        return "?";
    }

    std::vector<MapLabel> map_mode_labels(const MapData& map, const GameData* game, const LabelRaster& raster,
                                          const LabelFont& font, MapMode mode)
    {
        switch (mode) {
        case MapMode::terrain:
        case MapMode::political:
            return has_game(game) ? country_names(map, *game, raster, font) : region_names(map, raster, font);
        case MapMode::regions:
            return region_names(map, raster, font);
        default:
            return {};
        }
    }

    std::vector<std::uint32_t> map_mode_label_keys(const MapData& map, const GameData* game, MapMode mode)
    {
        std::vector<std::uint32_t> keys(map.count, 0);
        const bool data = has_game(game);
        for (std::uint32_t id = 1; id < map.count; ++id) {
            if (map.kind[id] != ProvinceKind::land) {
                continue;
            }
            switch (mode) {
            case MapMode::terrain:
            case MapMode::political:
                keys[id] = data ? game->state.provinces.owner[id] : map.region[id];
                break;
            case MapMode::regions: keys[id] = map.region[id]; break;
            default: break;
            }
        }
        return keys;
    }

    void map_mode_colors(const MapData& map, const GameData* game, MapMode mode, std::vector<ProvinceFill>* fills,
                         std::vector<BorderStyle>* styles)
    {
        const bool data = has_game(game);
        const bool by_region = !data || mode == MapMode::regions;

        fills->assign(map.count, ProvinceFill{});
        for (std::uint32_t id = 1; id < map.count; ++id) {
            if (map.kind[id] == ProvinceKind::wasteland && mode != MapMode::terrain) {
                (*fills)[id] = kWasteland;   // the terrain mode shows its true look, like the rest of the map
                continue;
            }
            if (map.kind[id] != ProvinceKind::land) {
                continue;
            }
            ProvinceFill& f = (*fills)[id];
            if (mode == MapMode::provinces) {
                f = rgba(map.color[id]);
                continue;
            }
            if (by_region) {
                f = region_color(map.region[id]);
                if (mode == MapMode::terrain) {
                    f.a = 0.0f;   // no fill, but country borders still take each side's colour, as in EU4
                }
                continue;
            }
            const Provinces& p = game->state.provinces;
            const std::uint16_t owner = p.owner[id];
            const ProvinceFill political = owner != 0 ? rgba(game->content.countries.color[owner]) : kNone;
            switch (mode) {
            case MapMode::terrain: f = political; f.a = 0.0f; break;
            case MapMode::political: f = political; break;
            case MapMode::terrain_type: f = listed(game->content.terrain, p.terrain[id]); break;
            case MapMode::rgo: f = listed(game->content.rgo_types.list, p.rgo[id]); break;
            case MapMode::unfilled: {
                const int missing = missing_fields(p, id);
                f = missing == 0 ? ProvinceFill{ 0.30f, 0.62f, 0.32f, 1.0f }
                                 : mix({ 0.95f, 0.62f, 0.20f, 1.0f }, { 0.80f, 0.10f, 0.10f, 1.0f },
                                       static_cast<float>(missing - 1) / static_cast<float>(kRequiredFields - 1));
                break;
            }
            default: break;
            }
        }

        const std::size_t chains = map.border_chains.size() / 4;
        styles->assign(chains, BorderStyle::none);
        for (std::size_t c = 0; c < chains; ++c) {
            const std::uint32_t pair = map.border_chains[c * 4 + 2];
            const std::uint32_t a = pair & 0xFFFF;
            const std::uint32_t b = pair >> 16;
            if (a >= map.count || b >= map.count) {
                continue;
            }
            const ProvinceKind ka = map.kind[a];
            const ProvinceKind kb = map.kind[b];
            auto dry = [](ProvinceKind k) { return k == ProvinceKind::land || k == ProvinceKind::wasteland; };
            if (dry(ka) && dry(kb)) {
                // A wasteland has no region and no owner: next to it every land province shows its realm's edge.
                const bool apart = by_region ? map.region[a] != map.region[b]
                                             : game->state.provinces.owner[a] != game->state.provinces.owner[b] ||
                                                   ka != kb;
                (*styles)[c] = apart ? BorderStyle::country : BorderStyle::province;
            } else if (ka == ProvinceKind::sea && kb == ProvinceKind::sea) {
                (*styles)[c] = BorderStyle::sea;
            }
        }
    }
}
