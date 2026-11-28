#include "ui/screens/dev_window.h"

#include <string>

#include <imgui.h>

#include "game/armies.h"
#include "game/game_data.h"
#include "game/session.h"
#include "game/war.h"

namespace mc
{
    void dev_window(const ScreenContext& ui, UiCommands* commands, bool* open)
    {
        ImGui::SetNextWindowSize(ImVec2(320.0f * ui.scale, 0.0f), ImGuiCond_FirstUseEver);
        if (!ImGui::Begin("Dev (F9)", open)) {
            ImGui::End();
            return;
        }
        const CountryDefinitions& countries = ui.content.countries;
        const Wars& wars = ui.state.wars;
        ImGui::TextUnformatted("Wars");
        bool any = false;
        for_each_alive(wars.alive, [&](std::uint32_t w) {
            any = true;
            ImGui::BulletText("%s attacks %s since %s", countries.name[wars.attacker[w]].c_str(),
                              countries.name[wars.defender[w]].c_str(), format(date_of(wars.start[w])).c_str());
        });
        if (!any) {
            ImGui::TextDisabled("None");
        }

        const std::uint16_t me = ui.session.player();
        ImGui::Separator();
        ImGui::TextUnformatted("Declare war");
        for (std::size_t c = 1; c < countries.size(); ++c) {
            if (c == me) {
                continue;
            }
            ImGui::PushID(static_cast<int>(c));
            ImGui::TextUnformatted(countries.name[c].c_str());
            ImGui::SameLine();
            if (at_war(ui.cache.at_war, me, c)) {
                ImGui::TextDisabled("(at war)");
            } else if (ImGui::SmallButton("Declare war")) {
                commands->declare_war = static_cast<std::uint16_t>(c);
            }
            ImGui::PopID();
        }
        ImGui::End();
    }
}
