#include "ui/screens/dev_window.h"

#include <algorithm>
#include <cstdio>
#include <string>

#include <imgui.h>

#include "core/money.h"
#include "game/armies.h"
#include "game/economy/budget.h"
#include "game/economy/taxes.h"
#include "game/game_data.h"
#include "game/peace.h"
#include "game/session.h"
#include "game/war.h"
#include "game/war_score.h"
#include "view/map_view.h"

namespace mc
{
    namespace
    {
        // "+26 (battles +3, occupied: +23 / -0)", or "+100 (complete victory)": from the attacker's side.
        std::string score_text(const WarScore& s)
        {
            if (s.complete != 0) {
                return std::to_string(s.total) + (s.total > 0 ? " (complete victory)" : " (complete defeat)");
            }
            char text[96];
            std::snprintf(text, sizeof text, "%+d (battles %+d, occupied %+d / -%d)", s.total, s.battles,
                          s.attacker_occupation, s.defender_occupation);
            return text;
        }

        // The peace with `enemy`: its provinces to tick, the price, the answer it would give, the two offers.
        void peace_with(const ScreenContext& ui, std::uint16_t me, std::uint16_t enemy, DevWindowState* state,
                        UiCommands* commands)
        {
            const Provinces& provinces = ui.state.provinces;
            std::erase_if(state->demand, [&](std::uint16_t p) { return provinces.owner[p] != enemy; });
            const CountryWarFacts facts = country_war_facts(ui.state, ui.content, enemy);
            if (ImGui::BeginChild("provinces", ImVec2(0.0f, 140.0f * ui.scale), ImGuiChildFlags_Borders)) {
                for (std::uint32_t id = 1; id < provinces.count; ++id) {
                    if (provinces.owner[id] != enemy) {
                        continue;
                    }
                    const auto p = static_cast<std::uint16_t>(id);
                    const auto at = std::find(state->demand.begin(), state->demand.end(), p);
                    bool ticked = at != state->demand.end();
                    const std::string label = std::string(ui.map.province_name(p)) + "##" + std::to_string(id);
                    if (ImGui::Checkbox(label.c_str(), &ticked)) {
                        if (ticked) {
                            state->demand.push_back(p);
                        } else {
                            state->demand.erase(at);
                        }
                    }
                    ImGui::SameLine();
                    ImGui::TextDisabled("%.1f%s", fx_to_double(province_value(ui.state, ui.content, p, facts)),
                                        provinces.controller[id] == me ? ", occupied" : "");
                }
            }
            ImGui::EndChild();

            const char* why = why_no_peace(ui.state, ui.cache.at_war, me, enemy, state->demand);
            const PeaceAnswer demand = peace_answer(ui.state, ui.content, me, enemy, state->demand);
            ImGui::Text("Cost %.1f of war score %d", fx_to_double(demand.cost), demand.score);
            ImGui::BeginDisabled(state->demand.empty() || why != nullptr);
            if (ImGui::SmallButton("Demand")) {
                commands->peace_with = enemy;
                commands->peace_provinces = state->demand;
                state->demand.clear();
            }
            ImGui::EndDisabled();
            ImGui::SameLine();
            ImGui::TextDisabled("%s", why != nullptr && !state->demand.empty() ? why
                                      : state->demand.empty()                 ? "(nothing ticked)"
                                      : demand.accepted                       ? "(would accept)"
                                                                              : "(would refuse)");
            const PeaceAnswer white = peace_answer(ui.state, ui.content, me, enemy, {});
            if (ImGui::SmallButton("White peace")) {
                commands->peace_with = enemy;
                commands->peace_provinces.clear();
            }
            ImGui::SameLine();
            ImGui::TextDisabled("%s", white.accepted ? "(would accept)" : "(would refuse)");
        }

        // The player's money (game/economy/budget.h): the treasury, the income, the inflation, the investments and
        // the budget sliders.
        void budget(const ScreenContext& ui, std::uint16_t me, DevWindowState* state, UiCommands* commands)
        {
            const WorldState& world = ui.state;
            auto cash = [&](std::size_t a) {
                return to_emeralds(world.money.cash[account(me, static_cast<Account>(a))]);
            };
            const ProvinceIncome income = country_income(world, ui.content, me);
            ImGui::Text("Treasury %.2f", cash(static_cast<std::size_t>(Account::treasury)));
            ImGui::Text("Monthly income %.3f (taxes)", income.tax);
            ImGui::Text("Census on 1 January %.2f", income.census);
            ImGui::Text("Inflation %.2f%%", static_cast<double>(world.countries.inflation[me]));

            const BudgetShares now = budget_shares(world.countries, me);
            if (!state->budget_edited) {
                state->budget = now;
            }
            constexpr const char* kLabels[kAccountsPerCountry] = { "Army", "Production", "Government", "Stability",
                                                                   "Treasury" };
            int sum = 0;
            ImGui::PushItemWidth(140.0f * ui.scale);
            for (std::size_t a = 0; a < kAccountsPerCountry; ++a) {
                int share = state->budget[a];
                if (ImGui::SliderInt(kLabels[a], &share, 0, 100, "%d%%")) {
                    state->budget[a] = static_cast<std::uint8_t>(share);
                    state->budget_edited = true;
                }
                if (static_cast<Account>(a) != Account::treasury) {
                    ImGui::SameLine();
                    ImGui::TextDisabled("%.2f invested", cash(a));
                }
                sum += state->budget[a];
            }
            ImGui::PopItemWidth();
            ImGui::BeginDisabled(sum != 100 || state->budget == now);
            if (ImGui::SmallButton("Apply")) {
                commands->set_budget = true;
                commands->budget = state->budget;
                state->budget_edited = false;
            }
            ImGui::EndDisabled();
            ImGui::SameLine();
            ImGui::BeginDisabled(!state->budget_edited);
            if (ImGui::SmallButton("Reset")) {
                state->budget_edited = false;
            }
            ImGui::EndDisabled();
            ImGui::SameLine();
            ImGui::TextDisabled(sum == 100 ? "(%d%%)" : "(%d%%: must be 100%%)", sum);
        }
    }

    void dev_window(const ScreenContext& ui, DevWindowState* state, UiCommands* commands, bool* open)
    {
        ImGui::SetNextWindowSize(ImVec2(360.0f * ui.scale, 0.0f), ImGuiCond_FirstUseEver);
        if (!ImGui::Begin("Dev (F9)", open)) {
            ImGui::End();
            return;
        }
        const CountryDefinitions& countries = ui.content.countries;
        const Wars& wars = ui.state.wars;
        const std::uint16_t me = ui.session.player();
        ImGui::TextUnformatted("Budget");
        budget(ui, me, state, commands);

        ImGui::Separator();
        ImGui::TextUnformatted("Wars");
        bool any = false;
        for_each_alive(wars.alive, [&](std::uint32_t w) {
            any = true;
            const std::uint16_t attacker = wars.attacker[w];
            const std::uint16_t defender = wars.defender[w];
            ImGui::BulletText("%s attacks %s since %s", countries.name[attacker].c_str(),
                              countries.name[defender].c_str(), format(date_of(wars.start[w])).c_str());
            const WarScore score = war_score(ui.state, ui.content, static_cast<std::uint16_t>(w));
            ImGui::Indent();
            ImGui::Text("War score %s", score_text(score).c_str());
            if (attacker == me || defender == me) {
                const std::uint16_t enemy = attacker == me ? defender : attacker;
                ImGui::PushID(static_cast<int>(w));
                if (ImGui::TreeNode("peace", "Peace with %s", countries.name[enemy].c_str())) {
                    peace_with(ui, me, enemy, state, commands);
                    ImGui::TreePop();
                }
                ImGui::PopID();
            }
            ImGui::Unindent();
        });
        if (!any) {
            ImGui::TextDisabled("None");
        }

        ImGui::Separator();
        ImGui::TextUnformatted("Declare war");
        const std::uint32_t today = day_number(ui.date);
        for (std::size_t c = 1; c < countries.size(); ++c) {
            if (c == me) {
                continue;
            }
            ImGui::PushID(static_cast<int>(c));
            ImGui::TextUnformatted(countries.name[c].c_str());
            ImGui::SameLine();
            const std::uint32_t truce = truce_until(ui.state.truces, me, static_cast<std::uint16_t>(c), today);
            if (at_war(ui.cache.at_war, me, c)) {
                ImGui::TextDisabled("(at war)");
            } else if (truce != 0) {
                ImGui::TextDisabled("(truce until %s)", format(date_of(truce)).c_str());
            } else if (ImGui::SmallButton("Declare war")) {
                commands->declare_war = static_cast<std::uint16_t>(c);
            }
            ImGui::PopID();
        }
        ImGui::End();
    }
}
