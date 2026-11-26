#include "ui/screens/army_plates.h"

#include <algorithm>
#include <cfloat>
#include <cmath>
#include <format>
#include <string>

#include <imgui.h>

#include "game/armies.h"
#include "game/game_data.h"
#include "game/session.h"
#include "ui/flags.h"

namespace mc
{
    namespace
    {
        // At UI scale 1, px.
        constexpr float kHeight = 20.0f;
        constexpr float kPad = 5.0f;
        constexpr float kBar = 5.0f;
        constexpr float kText = 16.0f;
        constexpr float kGap = 3.0f;   // below the feet

        enum class Relation
        {
            own,
            neutral,
            enemy,   // none before wars (M7)
        };

        ImU32 background(Relation r)
        {
            switch (r) {
            case Relation::own: return IM_COL32(46, 110, 52, 235);
            case Relation::enemy: return IM_COL32(140, 36, 36, 235);
            case Relation::neutral: break;
            }
            return IM_COL32(72, 72, 78, 235);
        }

        // The box being dragged to select armies: a light veil with a white edge.
        void selection_box(ImDrawList* draw, const SelectionBox& box, float scale)
        {
            if (!box.shown) {
                return;
            }
            const ImVec2 lo(std::min(box.x0, box.x1), std::min(box.y0, box.y1));
            const ImVec2 hi(std::max(box.x0, box.x1), std::max(box.y0, box.y1));
            draw->AddRectFilled(lo, hi, IM_COL32(255, 255, 255, 36));
            draw->AddRect(lo, hi, IM_COL32(255, 255, 255, 210), 0.0f, 0, std::max(1.0f, scale));
        }

        // "800", "1.0k", "12.5k": the men as EU4 writes them.
        std::string men_text(float men)
        {
            if (men < 1000.0f) {
                return std::format("{:.0f}", men);
            }
            return std::format("{:.1f}k", men / 1000.0f);
        }
    }

    int army_plates(const ScreenContext& ui, const ArmyMarkers& armies, float cursor_x, float cursor_y)
    {
        ImDrawList* draw = ImGui::GetBackgroundDrawList();
        int under = -1;
        if (!ui.loaded || armies.markers.empty() || armies.fade <= 0.0f) {
            selection_box(draw, armies.box, ui.scale);
            return under;
        }
        // Whose armies are the player's: the country played, or, while choosing, the one selected.
        const std::uint16_t me = ui.session.playing() ? ui.session.player() : ui.session.selected();
        ImFont* font = ui.bold ? ui.bold : ImGui::GetFont();
        const float s = ui.scale;
        const float h = kHeight * s;
        const float text_size = kText * s;
        // The fade as the draw list's alpha.
        const float alpha = std::min(armies.fade, 1.0f);
        auto faded = [&](ImU32 c) {
            const auto a = static_cast<ImU32>(((c >> IM_COL32_A_SHIFT) & 0xFF) * alpha);
            return (c & ~IM_COL32_A_MASK) | (a << IM_COL32_A_SHIFT);
        };
        for (const ArmyMarker& marker : armies.markers) {
            const std::uint16_t owner = ui.state.armies.owner[marker.army];
            const ArmyTotals totals = army_totals(ui.state.regiments, marker.army);
            const float full = owner < ui.state.countries.count ? ui.state.countries.morale[owner] : 0.0f;
            const float morale = full > 0.0f ? std::clamp(totals.morale / full, 0.0f, 1.0f) : 0.0f;
            const Relation relation = owner != 0 && owner == me ? Relation::own : Relation::neutral;

            const std::string text = men_text(totals.men);
            const ImVec2 text_extent = font->CalcTextSizeA(text_size, FLT_MAX, 0.0f, text.c_str());
            const float flag = h - 2.0f * s;
            const float width = s + flag + kPad * s + text_extent.x + kPad * s + kBar * s + 2.0f * s;
            const ImVec2 lo(std::floor(marker.x - width * 0.5f), std::floor(marker.y + kGap * s));
            const ImVec2 hi(lo.x + width, lo.y + h);

            if (cursor_x >= lo.x && cursor_x < hi.x && cursor_y >= lo.y && cursor_y < hi.y) {
                under = marker.army;   // the later plates lie on top
            }
            const bool selected = std::ranges::binary_search(armies.selected, marker.army);
            draw->AddRectFilled(lo, hi, faded(background(relation)), 2.0f * s);
            if (selected) {
                const float frame = std::max(2.0f, 2.0f * s);
                draw->AddRect(ImVec2(lo.x - frame * 0.5f, lo.y - frame * 0.5f),
                              ImVec2(hi.x + frame * 0.5f, hi.y + frame * 0.5f), faded(IM_COL32(255, 255, 255, 255)),
                              2.0f * s, 0, frame);
            } else {
                draw->AddRect(lo, hi, faded(IM_COL32(0, 0, 0, 220)), 2.0f * s, 0, std::max(1.0f, s));
            }
            if (owner != 0) {
                ui.flags.draw_at(draw, ui.content.countries, owner, ImVec2(lo.x + s, lo.y + s), flag, alpha);
            }
            const ImVec2 text_at(lo.x + s + flag + kPad * s, lo.y + (h - text_extent.y) * 0.5f);
            draw->AddText(font, text_size, ImVec2(text_at.x + s, text_at.y + s), faded(IM_COL32(0, 0, 0, 200)),
                          text.c_str());
            draw->AddText(font, text_size, text_at, faded(IM_COL32(240, 240, 232, 255)), text.c_str());

            // The morale bar, filling from the bottom.
            const ImVec2 bar_lo(hi.x - 2.0f * s - kBar * s, lo.y + 2.0f * s);
            const ImVec2 bar_hi(hi.x - 2.0f * s, hi.y - 2.0f * s);
            draw->AddRectFilled(bar_lo, bar_hi, faded(IM_COL32(20, 20, 20, 230)));
            const float top = bar_hi.y - (bar_hi.y - bar_lo.y) * morale;
            draw->AddRectFilled(ImVec2(bar_lo.x, top), bar_hi, faded(IM_COL32(110, 200, 60, 255)));
        }
        selection_box(draw, armies.box, s);
        return under;
    }
}
