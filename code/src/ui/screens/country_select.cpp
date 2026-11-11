#include "ui/screens/country_select.h"

#include <cfloat>
#include <string>

#include <imgui.h>

#include "game/game_data.h"
#include "core/map_data.h"
#include "game/countries.h"
#include "game/session.h"
#include "ui/flags.h"
#include "ui/widgets.h"

namespace mc
{
    namespace
    {
        constexpr float kMargin = 12.0f;          // px from the screen edges at UI scale 1
        constexpr float kSummaryWidth = 360.0f;
        constexpr float kFlagSize = 96.0f;

        void summary(const ScreenContext& ui, std::uint16_t country)
        {
            const CountryDefinitions& countries = ui.content.countries;
            ui.flags.draw(countries, country, kFlagSize * ui.scale);
            ImGui::SameLine();
            ImGui::BeginGroup();
            ui::big_text(countries.name[country], 1.45f);
            ImGui::TextDisabled("%s  -  %s", countries.tag_text(country).c_str(), countries.adjective[country].c_str());
            ImGui::EndGroup();
            ImGui::Separator();

            const CountryStats stats = country_stats(ui.state, country);
            if (!ui::begin_facts("summary")) {
                return;
            }
            ui::fact("Capital");
            const std::uint16_t capital = ui.state.countries.capital[country];
            if (capital != 0) {
                ImGui::TextUnformatted(std::string(ui.map.province_name(capital)).c_str());
            } else {
                ImGui::TextDisabled("none");
            }
            ui::fact("Primary culture");
            ui::listed(ui.content.cultures, ui.state.countries.primary_culture[country], &ui.content.culture_groups);
            ui::fact("State religion");
            ui::listed(ui.content.religions, ui.state.countries.religion[country], &ui.content.religion_groups);
            ui::fact("Provinces");
            ImGui::Text("%zu", stats.provinces);
            if (stats.cores > 0) {
                ImGui::SameLine();
                ImGui::TextDisabled("(cores on %zu)", stats.cores);
            }
            ui::fact("Development");
            ImGui::Text("%.0f", stats.development());
            ImGui::SameLine();
            ImGui::TextDisabled("%.0f / %.0f / %.0f", stats.tax, stats.production, stats.manpower);
            ui::end_facts();
        }
    }

    void country_select_screen(const ScreenContext& ui, UiCommands* commands)
    {
        const float margin = kMargin * ui.scale;
        const std::uint16_t selected = ui.session.selected();

        ImGui::SetNextWindowPos(ImVec2(ui.width - margin, margin), ImGuiCond_Always, ImVec2(1.0f, 0.0f));
        ImGui::SetNextWindowSizeConstraints(ImVec2(kSummaryWidth * ui.scale, 0.0f),
                                            ImVec2(kSummaryWidth * ui.scale, FLT_MAX));
        if (ImGui::Begin("Choose your country", nullptr, ui::kFixedWindow)) {
            if (!ui.loaded) {
                ImGui::TextWrapped("No game data (data/): nothing to play.");
            } else if (selected == 0) {
                ImGui::TextWrapped("Click a country on the map.");
            } else {
                summary(ui, selected);
            }
        }
        ImGui::End();

        ImGui::SetNextWindowPos(ImVec2(ui.width - margin, ui.height - margin), ImGuiCond_Always, ImVec2(1.0f, 1.0f));
        if (ImGui::Begin("##play", nullptr, ui::kFixedWindow | ImGuiWindowFlags_NoTitleBar)) {
            ImGui::BeginDisabled(selected == 0);
            ImGui::PushFont(nullptr, ImGui::GetStyle().FontSizeBase * 1.5f);
            if (ImGui::Button("Play", ImVec2(200.0f * ui.scale, 52.0f * ui.scale))) {
                commands->play = true;
            }
            ImGui::PopFont();
            ImGui::EndDisabled();
        }
        ImGui::End();
    }

    void country_tooltip(const ScreenContext& ui, std::uint32_t province, float cursor_x, float cursor_y)
    {
        if (!ui.loaded || province == 0 || province >= ui.state.provinces.count) {
            return;
        }
        const std::uint16_t owner = ui.state.provinces.owner[province];
        if (owner == 0) {
            return;
        }
        // Below and right of the OS cursor (ImGui does not know its size).
        ImGui::SetNextWindowPos(ImVec2(cursor_x + 16.0f * ui.scale, cursor_y + 20.0f * ui.scale));
        if (ImGui::BeginTooltip()) {
            ui::swatch(ui.content.countries.color[owner]);
            ImGui::TextUnformatted(ui.content.countries.name[owner].c_str());
            ImGui::TextDisabled("%s", std::string(ui.map.province_name(province)).c_str());
            ImGui::EndTooltip();
        }
    }
}
