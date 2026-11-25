#include "ui/screens/province_tooltip.h"

#include <string>
#include <vector>

#include <imgui.h>

#include "game/armies.h"
#include "game/game_data.h"
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
            ImGui::Text("%u regiments, %.0f men", totals.regiments, totals.men);
            ImGui::Text("Morale %.2f / %.2f", totals.morale, ui.state.countries.morale[army_owner]);
        }
        ImGui::EndTooltip();
    }
}
