#include "ui/screens/time_bar.h"

#include <string>

#include <imgui.h>

#include "game/pace.h"
#include "game/session.h"
#include "ui/widgets.h"

namespace mc
{
    namespace
    {
        constexpr float kMargin = 12.0f;        // px from the screen edges at UI scale 1
        constexpr float kPauseWidth = 76.0f;    // fits "Resume": the buttons do not shift when it toggles
        constexpr float kSpeedWidth = 28.0f;
    }

    void time_bar(const ScreenContext& ui, UiCommands* commands)
    {
        const Pace& pace = ui.session.pace();
        const float margin = kMargin * ui.scale;
        ImGui::SetNextWindowPos(ImVec2(ui.width - margin, margin), ImGuiCond_Always, ImVec2(1.0f, 0.0f));
        if (ImGui::Begin("##time", nullptr, ui::kFixedWindow | ImGuiWindowFlags_NoTitleBar)) {
            ui::big_text(format(ui.date), 1.25f);

            if (ImGui::Button(pace.paused() ? "Resume" : "Pause", ImVec2(kPauseWidth * ui.scale, 0.0f))) {
                commands->toggle_pause = true;
            }
            ImGui::SameLine();
            ImGui::BeginDisabled(pace.speed() == Pace::kSlowest);
            if (ImGui::Button("-", ImVec2(kSpeedWidth * ui.scale, 0.0f))) {
                commands->set_speed = pace.speed() - 1;
            }
            ImGui::EndDisabled();
            for (int speed = Pace::kSlowest; speed <= Pace::kFastest; ++speed) {
                ImGui::SameLine();
                const bool current = speed == pace.speed();
                if (current) {
                    ImGui::PushStyleColor(ImGuiCol_Button, ImGui::GetStyleColorVec4(ImGuiCol_ButtonActive));
                }
                if (ImGui::Button(std::to_string(speed).c_str(), ImVec2(kSpeedWidth * ui.scale, 0.0f))) {
                    commands->set_speed = speed;
                }
                if (current) {
                    ImGui::PopStyleColor();
                }
            }
            ImGui::SameLine();
            ImGui::BeginDisabled(pace.speed() == Pace::kFastest);
            if (ImGui::Button("+", ImVec2(kSpeedWidth * ui.scale, 0.0f))) {
                commands->set_speed = pace.speed() + 1;
            }
            ImGui::EndDisabled();
        }
        ImGui::End();
    }
}
