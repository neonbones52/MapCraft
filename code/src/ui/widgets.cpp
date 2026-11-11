#include "ui/widgets.h"

#include "game/game_data.h"

namespace mc::ui
{
    ImVec4 color(std::uint32_t rgba, float alpha)
    {
        return { (rgba & 255) / 255.0f, ((rgba >> 8) & 255) / 255.0f, ((rgba >> 16) & 255) / 255.0f, alpha };
    }

    void swatch(std::uint32_t rgba)
    {
        const float h = ImGui::GetTextLineHeight();
        ImGui::ColorButton("##swatch", color(rgba), ImGuiColorEditFlags_NoTooltip | ImGuiColorEditFlags_NoDragDrop,
                           ImVec2(h, h));
        ImGui::SameLine();
    }

    void big_text(std::string_view text, float factor)
    {
        ImGui::PushFont(nullptr, ImGui::GetStyle().FontSizeBase * factor);
        ImGui::TextUnformatted(text.data(), text.data() + text.size());
        ImGui::PopFont();
    }

    bool begin_facts(const char* id)
    {
        if (!ImGui::BeginTable(id, 2, ImGuiTableFlags_SizingStretchProp)) {
            return false;
        }
        ImGui::TableSetupColumn("label", ImGuiTableColumnFlags_WidthFixed);
        ImGui::TableSetupColumn("value", ImGuiTableColumnFlags_WidthStretch);
        return true;
    }

    void fact(const char* label)
    {
        ImGui::TableNextRow();
        ImGui::TableSetColumnIndex(0);
        ImGui::TextDisabled("%s", label);
        ImGui::TableSetColumnIndex(1);
    }

    void end_facts()
    {
        ImGui::EndTable();
    }

    void listed(const DefinitionList& list, std::size_t index, const DefinitionList* groups)
    {
        if (index == 0 || index >= list.size()) {
            ImGui::TextDisabled("-");
            return;
        }
        swatch(list.color[index]);
        ImGui::TextUnformatted(list.name[index].c_str());
        if (groups && list.group[index] != 0) {
            ImGui::SameLine();
            ImGui::TextDisabled("(%s)", groups->name[list.group[index]].c_str());
        }
    }
}
