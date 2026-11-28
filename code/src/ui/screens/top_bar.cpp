#include "ui/screens/top_bar.h"

#include <format>
#include <string>

#include <imgui.h>

#include "core/money.h"
#include "game/economy/income.h"
#include "game/economy/manpower.h"
#include "game/game_data.h"
#include "game/session.h"
#include "ui/banners.h"
#include "ui/mc_widgets.h"
#include "ui/windows.h"

namespace mc
{
    namespace
    {
        // The banner's pixel, screen px: one more than the GUI scale (60 x 120 at 1080).
        float banner_texel()
        {
            return static_cast<float>(mcui::scale() + 1);
        }

        // The banner's panel: the banner and the panel's padding (7 and 6 GUI px, mcui::begin_panel).
        ImVec2 banner_panel_size()
        {
            const float s = static_cast<float>(mcui::scale());
            return ImVec2(20.0f * banner_texel() + 14.0f * s, 40.0f * banner_texel() + 12.0f * s);
        }

        // "6.9k", "13.2k", "950".
        std::string men(std::int32_t n)
        {
            return n >= 1000 ? std::format("{:.1f}k", n / 1000.0) : std::to_string(n);
        }

        // An icon and its value on the bar, the value's text centred on the icon; a tooltip while hovered.
        template <class Tooltip>
        void stat(std::string_view item, const std::string& value, Tooltip&& tooltip)
        {
            ImGui::SameLine();
            ImGui::BeginGroup();
            mcui::item_icon(item);
            ImGui::SameLine(0.0f, static_cast<float>(2 * mcui::scale()));
            const float icon = static_cast<float>(16 * mcui::scale());
            ImGui::SetCursorPosY(ImGui::GetCursorPosY() + (icon - ImGui::GetTextLineHeight()) * 0.5f);
            mcui::text(value);
            ImGui::EndGroup();
            if (ImGui::IsItemHovered() && mcui::begin_tooltip()) {
                tooltip();
                mcui::end_tooltip();
            }
        }

        void line(const std::string& text, bool first = false)
        {
            mcui::text(text, first ? mcui::kWhite : mcui::kDisabledText, true);
        }
    }

    float screen_margin()
    {
        return static_cast<float>(4 * mcui::scale());
    }

    float left_zone_top()
    {
        return screen_margin() + banner_panel_size().y + static_cast<float>(2 * mcui::scale());
    }

    void top_bar(const ScreenContext& ui, BannerCache* banners, Windows* windows)
    {
        const bool playing = ui.session.playing() && ui.loaded;
        const std::uint16_t me = playing ? ui.session.player() : 0;
        const float margin = screen_margin();
        const ImVec2 corner = banner_panel_size();

        ImGui::SetNextWindowPos(ImVec2(margin, margin), ImGuiCond_Always);
        ImGui::SetNextWindowSize(corner, ImGuiCond_Always);
        if (mcui::begin_panel("##banner", nullptr, mcui::kFixedPanel)) {
            const SpriteImage* banner = me != 0 ? banners->country(ui.content.countries, me) : banners->observer();
            const ImVec2 size(20.0f * banner_texel(), 40.0f * banner_texel());
            if (mcui::image_button("##banner button", banner, size) && me != 0) {
                windows->toggle(GameWindow::country);
            }
            if (me != 0 && ImGui::IsItemHovered() && mcui::begin_tooltip()) {
                line(ui.content.countries.name[me], true);
                line(windows->is_open(GameWindow::country) ? "Click to close the country window"
                                                           : "Click to open the country window");
                mcui::end_tooltip();
            }
        }
        mcui::end_panel();
        if (me == 0) {
            return;
        }

        const WorldState& state = ui.state;
        const float s = static_cast<float>(mcui::scale());
        ImGui::SetNextWindowPos(ImVec2(margin + corner.x + 2.0f * s, margin), ImGuiCond_Always);
        if (mcui::begin_panel("##top bar", nullptr, mcui::kFixedPanel | ImGuiWindowFlags_AlwaysAutoResize)) {
            const float icon = 16.0f * s;
            ImGui::BeginGroup();   // the name centred on the icons' height, the icons back on the line
            ImGui::SetCursorPosY(ImGui::GetCursorPosY() + (icon - ImGui::GetTextLineHeight()) * 0.5f);
            mcui::text(ui.content.countries.name[me]);
            ImGui::EndGroup();

            const ProvinceIncome income = country_income(state, ui.content, me);
            const double treasury = to_emeralds(state.money.cash[account(me, Account::treasury)]);
            stat("emerald", std::format("{:.2f} ({:+.2f})", treasury, income.monthly()), [&] {
                line(std::format("Treasury: {:.2f}", treasury), true);
                line(std::format("Monthly income: {:+.2f}", income.monthly()));
                line(std::format("  taxes {:.2f}, production {:.2f}, emeralds {:.2f}", income.tax, income.production,
                                 income.gold));
                line(std::format("Census on 1 January: {:.2f}", income.census));
            });

            const std::int32_t maximum = max_manpower(state, ui.content, me);
            stat("iron_helmet", men(state.countries.manpower[me]), [&] {
                line(std::format("Manpower: {} of {}", state.countries.manpower[me], maximum), true);
                line(std::format("Recovers {} a month", monthly_manpower(ui.content.economy, maximum)));
            });

            const double inflation = static_cast<double>(state.countries.inflation[me]);
            stat("gold_nugget", std::format("{:.2f}%", inflation), [&] {
                line(std::format("Inflation: {:.2f}%", inflation), true);
                line("Everything costs this much more");
            });
        }
        mcui::end_panel();
    }
}
