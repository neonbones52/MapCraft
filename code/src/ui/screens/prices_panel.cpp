#include "ui/screens/prices_panel.h"

#include <imgui.h>

#include "game/game_data.h"
#include "ui/widgets.h"

namespace mc
{
    namespace
    {
        constexpr float kMargin = 12.0f;   // px from the screen edges at UI scale 1
    }

    void prices_panel(const ScreenContext& ui, UiCommands* commands)
    {
        const DefinitionList& goods = ui.content.goods;
        if (goods.size() <= 1 || ui.content.markets.size() <= 1) {
            return;
        }
        const float margin = kMargin * ui.scale;
        ImGui::SetNextWindowPos(ImVec2(margin, ui.height - margin), ImGuiCond_Always, ImVec2(0.0f, 1.0f));
        if (ImGui::Begin("Prices", nullptr, ui::kFixedWindow)) {
            if (ImGui::RadioButton("Off", ui.price_good == 0) && ui.price_good != 0) {
                commands->show_prices = 0;
            }
            for (std::size_t g = 1; g < goods.size(); ++g) {
                if (goods.key[g] == kMoneyGood) {
                    continue;
                }
                const bool on = ui.price_good == g;
                if (ImGui::RadioButton(goods.name[g].c_str(), on) && !on) {
                    commands->show_prices = static_cast<int>(g);
                }
            }
        }
        ImGui::End();
    }
}
