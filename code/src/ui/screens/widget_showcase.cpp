#include "ui/screens/widget_showcase.h"

#include <format>
#include <string>

#include <imgui.h>

#include "ui/mc_widgets.h"

namespace mc
{
    void widget_showcase(ShowcaseState* state, bool on_panel)
    {
        const ImU32 ink = on_panel ? mcui::kPanelText : mcui::kWhite;
        const bool shadow = !on_panel;
        auto heading = [&](const char* text) {
            ImGui::Spacing();
            mcui::text(text, ink, shadow);
        };

        heading("Buttons: highlighted while held down; let go elsewhere and nothing happens");
        if (mcui::button("Button", 100)) {
            ++state->clicks;
        }
        ImGui::SameLine();
        mcui::button("Disabled", 100, false);
        ImGui::SameLine();
        mcui::text(std::format("clicked {} times", state->clicks), ink, shadow);

        heading("Checkboxes");
        mcui::checkbox("Checked", &state->checked);
        ImGui::SameLine();
        mcui::checkbox("Unchecked", &state->unchecked);

        heading("Slider");
        mcui::slider_int("Treasury", &state->slider, 0, 100, "%d%%", 200);

        heading("Tabs");
        constexpr const char* kTabs[] = { "Economy", "Army", "Diplomacy" };
        for (int t = 0; t < 3; ++t) {
            if (t > 0) {
                ImGui::SameLine(0.0f, 0.0f);
            }
            if (mcui::tab(kTabs[t], state->tab == t, 70)) {
                state->tab = t;
            }
        }

        heading("A list that scrolls (drag the scroller, or the wheel)");
        if (mcui::begin_list("showcase list", 60)) {
            for (int i = 1; i <= 30; ++i) {
                mcui::text(std::format("Province {}", i), mcui::kWhite, true);
            }
        }
        mcui::end_list();

        heading("Tooltips (hover a slot)");
        // A slot and its tooltip: a name and a line under it.
        auto slot = [](const char* item, int count, const char* name, const char* line) {
            mcui::slot(item, count);
            if (ImGui::IsItemHovered() && mcui::begin_tooltip()) {
                mcui::text(name, mcui::kWhite, true);
                mcui::text(line, mcui::kDisabledText, true);
                mcui::end_tooltip();
            }
        };
        slot("emerald", 64, "Emerald", "Worth 1 emerald");
        ImGui::SameLine(0.0f, 0.0f);
        slot("iron_sword", 0, "Iron Sword", "Infantry's arms");
        ImGui::SameLine(0.0f, 0.0f);
        slot("wheat", 12, "Wheat", "A trade good");
        ImGui::SameLine(0.0f, 0.0f);
        slot("", 0, "Empty slot", "Nothing here");

        heading("Bars (morale)");
        mcui::bar(0.75f, mcui::BarColor::red, 182);
        mcui::bar(0.5f, mcui::BarColor::green, 182);
        mcui::bar(0.3f, mcui::BarColor::yellow, 120);
        mcui::bar(0.9f, mcui::BarColor::blue, 120);
        mcui::bar(0.6f, mcui::BarColor::white, 60);

        heading("Text");
        mcui::text("White with a shadow, as on Minecraft's buttons", mcui::kWhite, true);
        mcui::text("Dark grey without one, as on its panels", mcui::kPanelText, false);

        heading("GUI scale (0: the game's own, by the window's height)");
        mcui::slider_int("GUI scale", &state->gui_scale, 0, 4, "%d", 120);
        if (!on_panel) {
            ImGui::Spacing();
            if (mcui::button(state->panel_open ? "Close the panel" : "Open on a panel", 120)) {
                state->panel_open = !state->panel_open;
            }
        }
    }

    void showcase_panel(ShowcaseState* state)
    {
        if (!state->panel_open) {
            return;
        }
        ImGui::SetNextWindowPos(ImVec2(80.0f, 80.0f), ImGuiCond_FirstUseEver);
        if (mcui::begin_panel("Widgets", &state->panel_open, ImGuiWindowFlags_AlwaysAutoResize)) {
            widget_showcase(state, true);
        }
        mcui::end_panel();
    }
}
