#pragma once

#include <cstddef>
#include <cstdint>
#include <string_view>

#include <imgui.h>

namespace mc
{
    struct DefinitionList;
}

// Small ImGui pieces the game's screens share.
namespace mc::ui
{
    // RGBA8 (r in the low byte, as GameData colours) as an ImGui colour.
    [[nodiscard]] ImVec4 color(std::uint32_t rgba, float alpha = 1.0f);

    // A square of the colour, one text line high, then SameLine.
    void swatch(std::uint32_t rgba);

    // Text in the window's font at `factor` times its size.
    void big_text(std::string_view text, float factor);

    // Two-column label/value table: begin, then row() before each value.
    bool begin_facts(const char* id);
    void fact(const char* label);
    void end_facts();

    // An entry of a culture/religion-like list with its swatch and group, "-" for none.
    void listed(const DefinitionList& list, std::size_t index, const DefinitionList* groups);

    // Fixed, auto-sized windows the game places itself.
    inline constexpr ImGuiWindowFlags kFixedWindow = ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoResize |
                                                     ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoSavedSettings |
                                                     ImGuiWindowFlags_AlwaysAutoResize;
}
