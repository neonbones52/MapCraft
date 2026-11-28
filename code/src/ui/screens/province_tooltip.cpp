#include "ui/screens/province_tooltip.h"

#include <string>
#include <vector>

#include <imgui.h>

#include "game/armies.h"
#include "game/game_data.h"
#include "game/modifiers.h"
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
            ImGui::Text("Morale %.2f / %.2f", fx_to_double(totals.morale),
                        fx_to_double(max_morale(ui.cache.country_modifiers, army_owner)));
            if (const std::uint16_t leader = armies.leader[army]; leader != 0) {
                const Leaders& leaders = ui.state.leaders;
                ImGui::Text("General: fire %u, shock %u, maneuver %u", unsigned{ leaders.fire[leader - 1u] },
                            unsigned{ leaders.shock[leader - 1u] }, unsigned{ leaders.maneuver[leader - 1u] });
            }
        }
        ImGui::EndTooltip();
    }
}
