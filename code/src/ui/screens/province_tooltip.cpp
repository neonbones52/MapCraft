#include "ui/screens/province_tooltip.h"

#include <algorithm>
#include <string>
#include <vector>

#include <imgui.h>

#include "game/armies.h"
#include "game/economy/upkeep.h"
#include "game/game_data.h"
#include "game/modifiers.h"
#include "game/occupation.h"
#include "ui/army_name.h"
#include "ui/widgets.h"
#include "view/map_view.h"

namespace mc
{
    void province_tooltip(const ScreenContext& ui, std::uint32_t province, float cursor_x, float cursor_y)
    {
        if (!ui.loaded || province == 0 || province >= ui.state.provinces.count) {
            return;
        }
        const Armies& armies = ui.state.armies;
        std::vector<std::uint16_t> here;
        for_each_alive(armies.alive, [&](std::uint32_t a) {
            if (armies.province[a] == province) {
                here.push_back(static_cast<std::uint16_t>(a));
            }
        });
        const std::uint16_t owner = ui.state.provinces.owner[province];
        if (owner == 0 && here.empty()) {
            return;
        }
        const CountryDefinitions& countries = ui.content.countries;
        // Below and right of the OS cursor (ImGui does not know its size).
        ImGui::SetNextWindowPos(ImVec2(cursor_x + 16.0f * ui.scale, cursor_y + 20.0f * ui.scale));
        if (!ImGui::BeginTooltip()) {
            return;
        }
        if (owner != 0) {
            ui::swatch(countries.color[owner]);
            ImGui::TextUnformatted(countries.name[owner].c_str());
        }
        ImGui::TextDisabled("%s", std::string(ui.map.province_name(province)).c_str());
        // Who controls it, its fort and garrison, the siege if one goes on (game/occupation.h).
        const Provinces& provinces = ui.state.provinces;
        const auto p16 = static_cast<std::uint16_t>(province);
        if (const std::uint16_t controller = provinces.controller[province]; owner != 0 && controller != owner) {
            if (controller != 0) {
                ui::swatch(countries.color[controller]);
            }
            ImGui::Text("Occupied by %s", controller != 0 ? countries.name[controller].c_str() : "no one");
        }
        if (owner != 0) {
            if (const int fort = fort_level(ui.state, ui.content, p16); fort > 0) {
                ImGui::Text("Fort level %d, garrison %d / %d", fort, provinces.garrison[province],
                            max_garrison(ui.content, fort));
            } else {
                ImGui::TextDisabled("No fort");
            }
        }
        for_each_alive(ui.state.sieges.alive, [&](std::uint32_t s) {
            const Sieges& sieges = ui.state.sieges;
            if (sieges.province[s] != province) {
                return;
            }
            const auto slot = static_cast<std::uint16_t>(s);
            const Besiegers b = besiegers(ui.state, ui.content, slot);
            const SiegeNumbers n = siege_numbers(ui.state, ui.content, ui.cache, has_port(ui.map, p16), slot, b);
            ImGui::Separator();
            ImGui::Text("Siege by %s, %lld men (%lld needed)", countries.name[sieges.country[s]].c_str(),
                        static_cast<long long>(b.men), static_cast<long long>(n.needed));
            ImGui::Text("Day %u of %d, progress %u / %d, breaches %u", unsigned{ sieges.days[s] }, n.phase_days,
                        unsigned{ provinces.siege_progress[province] }, n.progress_cap, unsigned{ sieges.breaches[s] });
            if (n.fort > 0) {
                ImGui::TextDisabled("Roll + %d: general %d, artillery %d, fort -%d, blockade %d, obsolete %d, "
                                    "garrison %d",
                                    n.impact, n.leader, n.artillery, n.fort, n.blockade, n.obsolete, n.low_garrison);
            } else {
                ImGui::TextDisabled("No fort: falls at the end of the phase");
            }
            constexpr const char* kResults[] = { "",           "Status quo",     "Disease outbreak",
                                                 "Walls breached", "Supplies shortage", "Food shortage",
                                                 "Water shortage", "Defenders desert" };
            // The next phase's chances (EU4's siege tooltip): the faces of the die that give each result.
            const SiegeChances chances =
                siege_chances(ui.content.combat, n, sieges.breaches[s], provinces.garrison[province]);
            const auto percent = [&](int faces) { return 100.0 * faces / std::max(chances.sides, 1); };
            ImGui::Text("Chance to take it next phase: %.0f%%", percent(chances.surrender));
            for (std::size_t r = 1; r < chances.result.size(); ++r) {
                if (chances.result[r] > 0) {
                    ImGui::TextDisabled("  %s %.0f%%", kResults[r], percent(chances.result[r]));
                }
            }
            if (sieges.roll[s] != 0) {
                ImGui::TextDisabled("Last phase: rolled %u, %s", unsigned{ sieges.roll[s] },
                                    kResults[static_cast<std::size_t>(sieges.result[s])]);
            }
        });
        // The battle in the province, if one goes on (game/combat.h).
        for_each_alive(ui.state.battles.alive, [&](std::uint32_t b) {
            const Battle& battle = ui.state.battles.list[b];
            if (battle.province != province) {
                return;
            }
            ImGui::Separator();
            ImGui::Text("Battle: %s attacks %s", countries.name[battle.sides[0].country].c_str(),
                        countries.name[battle.sides[1].country].c_str());
            if (battle.phase_day == kNotDeployed) {
                ImGui::TextDisabled("Drawing up the lines");
            } else {
                ImGui::Text("Day %u, %s phase, dice %u : %u", unsigned{ battle.duration },
                            battle.phase == kPhaseFire ? "fire" : "shock", unsigned{ battle.sides[0].dice },
                            unsigned{ battle.sides[1].dice });
            }
            // The attacker's minus (game/combat.h): the terrain's and the river's (lifted by a better maneuver).
            const std::uint8_t terrain = ui.state.provinces.terrain[province];
            const int penalty = terrain < ui.content.terrain_attacker_penalty.size()
                                    ? ui.content.terrain_attacker_penalty[terrain]
                                    : 0;
            if (penalty != 0 || battle.crossing != 0) {
                ImGui::TextDisabled("Attacker: terrain -%d, river crossing -%d", penalty, int{ battle.crossing });
            }
        });
        for (const std::uint16_t army : here) {
            const std::uint16_t army_owner = armies.owner[army];
            const ArmyTotals totals = army_totals(ui.state.regiments, army);
            ImGui::Separator();
            if (army_owner != 0) {
                ui::swatch(countries.color[army_owner]);
            }
            ImGui::Text("%s", ui::army_name(army).c_str());
            if (army_owner != 0 && army_owner != owner) {
                ImGui::SameLine();
                ImGui::TextDisabled("(%s)", countries.name[army_owner].c_str());
            }
            ImGui::Text("%u regiments, %lld men", totals.regiments, static_cast<long long>(totals.men));
            // Below full maintenance, no more than its ceiling (game/economy/upkeep.h).
            const Fixed ceiling =
                morale_ceiling(ui.cache.country_modifiers, ui.state.countries, ui.content.economy, army_owner);
            ImGui::Text("Morale %.2f / %.2f", fx_to_double(std::min(totals.morale, ceiling)),
                        fx_to_double(max_morale(ui.cache.country_modifiers, army_owner)));
            if (const std::uint16_t leader = armies.leader[army]; leader != 0) {
                const Leaders& leaders = ui.state.leaders;
                ImGui::Text("General: fire %u, shock %u, maneuver %u, siege %u", unsigned{ leaders.fire[leader - 1u] },
                            unsigned{ leaders.shock[leader - 1u] }, unsigned{ leaders.maneuver[leader - 1u] },
                            unsigned{ leaders.siege[leader - 1u] });
            }
        }
        ImGui::EndTooltip();
    }
}
