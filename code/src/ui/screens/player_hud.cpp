#include "ui/screens/player_hud.h"

#include <imgui.h>

#include "game/game_data.h"
#include "game/session.h"
#include "ui/flags.h"
#include "ui/widgets.h"

namespace mc
{
    namespace
    {
        constexpr float kMargin = 12.0f;
        constexpr float kFlagSize = 48.0f;
    }

    void player_hud(const ScreenContext& ui, UiCommands* commands)
    {
        const std::uint16_t player = ui.session.player();
        const float margin = kMargin * ui.scale;
        ImGui::SetNextWindowPos(ImVec2(margin, margin), ImGuiCond_Always);
        if (ImGui::Begin("##player", nullptr, ui::kFixedWindow | ImGuiWindowFlags_NoTitleBar)) {
            ui.flags.draw(ui.content.countries, player, kFlagSize * ui.scale);
            ImGui::SameLine();
            ImGui::BeginGroup();
            ui::big_text(ui.content.countries.name[player], 1.25f);
            if (ImGui::SmallButton("Choose another country")) {
                commands->new_game = true;
            }
            ImGui::EndGroup();
        }
        ImGui::End();
    }
}
