#include "ui/screens/date_bar.h"

#include <format>

#include <imgui.h>

#include "ui/mc_widgets.h"
#include "ui/screens/top_bar.h"
#include "view/pace.h"

namespace mc
{
    void date_bar(const ScreenContext& ui, UiCommands* commands)
    {
        const Pace& pace = ui.pace;
        const float margin = screen_margin();
        ImGui::SetNextWindowPos(ImVec2(ui.width - margin, margin), ImGuiCond_Always, ImVec2(1.0f, 0.0f));
        if (mcui::begin_panel("##date", nullptr, mcui::kFixedPanel | ImGuiWindowFlags_AlwaysAutoResize)) {
            mcui::text(format(ui.date));
            // Fixed widths: nothing shifts when the pause toggles.
            if (mcui::button(pace.paused() ? "Resume" : "Pause", 50)) {
                commands->toggle_pause = true;
            }
            ImGui::SameLine();
            if (mcui::button("-", 16, pace.speed() > Pace::kSlowest)) {
                commands->set_speed = pace.speed() - 1;
            }
            ImGui::SameLine();
            // A group: the text centred on the buttons' height, the next button back on their line.
            const float button = static_cast<float>(20 * mcui::scale());
            ImGui::BeginGroup();
            ImGui::SetCursorPosY(ImGui::GetCursorPosY() + (button - ImGui::GetTextLineHeight()) * 0.5f);
            mcui::text(std::format("Speed {}", pace.speed()));
            ImGui::EndGroup();
            ImGui::SameLine();
            if (mcui::button("+", 16, pace.speed() < Pace::kFastest)) {
                commands->set_speed = pace.speed() + 1;
            }
        }
        mcui::end_panel();
    }
}
