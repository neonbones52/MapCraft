#include "ui/screens/country_window.h"

#include <format>
#include <string>

#include <imgui.h>

#include "core/money.h"
#include "game/armies.h"
#include "game/countries.h"
#include "game/economy/budget.h"
#include "game/economy/income.h"
#include "game/economy/manpower.h"
#include "game/economy/upkeep.h"
#include "game/game_data.h"
#include "game/session.h"
#include "game/war_score.h"
#include "ui/mc_widgets.h"
#include "ui/screens/top_bar.h"
#include "ui/windows.h"
#include "view/map_view.h"

namespace mc
{
    namespace
    {
        constexpr int kWidth = 250;   // GUI px
        constexpr const char* kTabNames[] = { "Government", "Economy", "Military", "Diplomacy" };
        static_assert(std::size(kTabNames) == static_cast<std::size_t>(CountryTab::count), "a name per tab");

        // A row: the label on the left, the value on the right.
        void fact(const std::string& label, const std::string& value)
        {
            mcui::text(label);
            const float width = ImGui::CalcTextSize(value.c_str()).x;
            ImGui::SameLine(ImGui::GetContentRegionMax().x - width);
            mcui::text(value);
        }

        void heading(const char* text)
        {
            ImGui::Spacing();
            mcui::text(text, mcui::kPanelText, false);
        }

        void government(const ScreenContext& ui, std::uint16_t me, UiCommands* commands)
        {
            const std::uint32_t capital = seat(ui.state, me);
            const CountryStats stats = country_stats(ui.state, me);
            heading(ui.content.countries.name[me].c_str());
            fact("Capital", capital != 0 ? std::string(ui.map.province_name(capital)) : std::string("none"));
            fact("Provinces", std::to_string(stats.provinces));
            fact("Cores", std::to_string(stats.cores));
            ImGui::Spacing();
            if (mcui::button("Play another country")) {
                commands->new_game = true;
            }
        }

        void economy(const ScreenContext& ui, std::uint16_t me)
        {
            const WorldState& state = ui.state;
            auto cash = [&](Account a) { return to_emeralds(state.money.cash[account(me, a)]); };
            const ProvinceIncome income = country_income(state, ui.content, me);
            heading("Treasury");
            fact("Treasury", std::format("{:.2f}", cash(Account::treasury)));
            fact("Monthly income", std::format("{:+.2f}", income.monthly()));
            fact("  Taxes", std::format("{:.2f}", income.tax));
            fact("  Production", std::format("{:.2f}", income.production));
            fact("  Emeralds", std::format("{:.2f}", income.gold));
            fact("Census on 1 January", std::format("{:.2f}", income.census));
            fact("Inflation", std::format("{:.2f}%", static_cast<double>(state.countries.inflation[me])));
            heading("Budget");
            const BudgetShares shares = budget_shares(state.countries, me);
            constexpr const char* kAccounts[] = { "Army", "Production", "Government", "Stability", "Treasury" };
            for (std::size_t a = 0; a < kAccountsPerCountry; ++a) {
                const auto kind = static_cast<Account>(a);
                fact(kAccounts[a], kind == Account::treasury
                                       ? std::format("{}%", shares[a])
                                       : std::format("{}%, invested {:.2f}", shares[a], cash(kind)));
            }
        }

        void military(const ScreenContext& ui, std::uint16_t me)
        {
            const WorldState& state = ui.state;
            const std::int32_t maximum = max_manpower(state, ui.content, me);
            const ArmyUpkeep army = army_upkeep(state, ui.content, me);
            std::uint32_t raising = 0;
            for_each_alive(state.recruits.alive,
                           [&](std::uint32_t r) { raising += state.recruits.country[r] == me ? 1u : 0u; });
            heading("Manpower");
            fact("Manpower", std::format("{} of {}", state.countries.manpower[me], maximum));
            fact("Recovery", std::format("+{} a month", monthly_manpower(ui.content.economy, maximum)));
            heading("Army");
            fact("Regiments", std::format("{} of {}", army.regiments, army.limit));
            fact("Being raised", std::to_string(raising));
            fact("Upkeep", std::format("{:.2f} a month", army.upkeep));
            fact("Maintenance", std::format("{}%", state.countries.maintenance[me]));
        }

        void diplomacy(const ScreenContext& ui, std::uint16_t me)
        {
            const Wars& wars = ui.state.wars;
            const CountryDefinitions& countries = ui.content.countries;
            heading("Wars");
            bool any = false;
            for_each_alive(wars.alive, [&](std::uint32_t w) {
                const std::uint16_t a = wars.attacker[w];
                const std::uint16_t d = wars.defender[w];
                if (a != me && d != me) {
                    return;
                }
                any = true;
                const std::uint16_t enemy = a == me ? d : a;
                fact(countries.name[enemy], std::format("war score {:+}", war_score_of(ui.state, ui.content, me,
                                                                                     enemy)));
            });
            if (!any) {
                mcui::text("At peace");
            }
        }
    }

    void country_window(const ScreenContext& ui, Windows* windows, CountryWindowState* state, UiCommands* commands)
    {
        const std::uint16_t me = ui.session.player();
        if (!windows->is_open(GameWindow::country) || me == 0) {
            return;
        }
        const float margin = screen_margin();
        const float top = left_zone_top();
        const float width = static_cast<float>(kWidth * mcui::scale());
        ImGui::SetNextWindowPos(ImVec2(margin, top), ImGuiCond_Always);
        ImGui::SetNextWindowSize(ImVec2(width, ui.height - top - margin), ImGuiCond_Always);
        if (mcui::begin_panel("Country", windows->flag(GameWindow::country), mcui::kFixedPanel)) {
            for (std::size_t t = 0; t < std::size(kTabNames); ++t) {
                if (t > 0) {
                    ImGui::SameLine(0.0f, 0.0f);
                }
                if (mcui::tab(kTabNames[t], state->tab == static_cast<CountryTab>(t))) {
                    state->tab = static_cast<CountryTab>(t);
                }
            }
            switch (state->tab) {
            case CountryTab::government:
                government(ui, me, commands);
                break;
            case CountryTab::economy:
                economy(ui, me);
                break;
            case CountryTab::military:
                military(ui, me);
                break;
            case CountryTab::diplomacy:
                diplomacy(ui, me);
                break;
            case CountryTab::count:
                break;
            }
        }
        mcui::end_panel();
    }
}
