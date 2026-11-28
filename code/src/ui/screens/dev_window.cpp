#include "ui/screens/dev_window.h"

#include <algorithm>
#include <array>
#include <cstdio>
#include <string>

#include <imgui.h>

#include "core/money.h"
#include "game/armies.h"
#include "game/economy/budget.h"
#include "game/economy/income.h"
#include "game/economy/manpower.h"
#include "game/economy/upkeep.h"
#include "game/game_data.h"
#include "game/countries.h"
#include "game/peace.h"
#include "game/recruitment.h"
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

        // The sliders `start` with slider `moved` set to `value` (0..100): the others share the rest, 100 - value, in
        // proportion to their shares in `start` (all of them 0: equally), each the floor of its part; the percents the
        // floors leave go one each to the largest remainders (the first of equals), so the sum is 100.
        BudgetShares rebalance(const BudgetShares& start, std::size_t moved, int value)
        {
            BudgetShares out{};
            out[moved] = static_cast<std::uint8_t>(value);
            const int rest = 100 - value;
            int others = 0;
            for (std::size_t a = 0; a < kAccountsPerCountry; ++a) {
                others += a == moved ? 0 : start[a];
            }
            const int count = static_cast<int>(kAccountsPerCountry) - 1;
            std::array<int, kAccountsPerCountry> remainder{};
            int given = 0;
            for (std::size_t a = 0; a < kAccountsPerCountry; ++a) {
                if (a == moved) {
                    remainder[a] = -1;
                    continue;
                }
                const int weight = others == 0 ? 1 : start[a];
                const int total = others == 0 ? count : others;
                out[a] = static_cast<std::uint8_t>(rest * weight / total);
                remainder[a] = rest * weight % total;
                given += out[a];
            }
            for (int left = rest - given; left > 0; --left) {
                std::size_t best = moved;
                for (std::size_t a = 0; a < kAccountsPerCountry; ++a) {
                    if (remainder[a] >= 0 && (best == moved || remainder[a] > remainder[best])) {
                        best = a;
                    }
                }
                ++out[best];
                remainder[best] = -1;
            }
            return out;
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
            ImGui::Text("Monthly income %.3f", income.monthly());
            ImGui::TextDisabled("  taxes %.3f, production %.3f, emeralds %.3f", income.tax, income.production,
                                income.gold);
            ImGui::Text("Census on 1 January %.2f", income.census);
            const ArmyUpkeep army = army_upkeep(world, ui.content, me);
            ImGui::Text("Army upkeep %.3f a month", army.upkeep);
            ImGui::TextDisabled("  %u regiments of a force limit of %u", army.regiments, army.limit);
            ImGui::Text("Inflation %.2f%%", static_cast<double>(world.countries.inflation[me]));

            // The sliders, as in EU3: moving one moves the others in proportion, so they always add up to 100; the
            // shares go to the world (the command budget) when the slider is let go.
            if (!state->budget_edited) {
                state->budget = budget_shares(world.countries, me);
            }
            constexpr const char* kLabels[kAccountsPerCountry] = { "Army", "Production", "Government", "Stability",
                                                                   "Treasury" };
            ImGui::PushItemWidth(140.0f * ui.scale);
            for (std::size_t a = 0; a < kAccountsPerCountry; ++a) {
                int share = state->budget[a];
                const bool moved = ImGui::SliderInt(kLabels[a], &share, 0, 100, "%d%%");
                if (ImGui::IsItemActivated()) {
                    state->budget_start = state->budget;
                    state->budget_edited = true;
                }
                if (moved) {
                    state->budget = rebalance(state->budget_start, a, share);
                }
                if (ImGui::IsItemDeactivated()) {
                    if (state->budget != budget_shares(world.countries, me)) {
                        commands->set_budget = true;
                        commands->budget = state->budget;
                    }
                    state->budget_edited = false;
                }
                if (static_cast<Account>(a) != Account::treasury) {
                    ImGui::SameLine();
                    ImGui::TextDisabled("%.2f invested", cash(a));
                }
            }
            // The army maintenance (game/economy/upkeep.h), set when let go.
            int maintenance = state->maintenance_edited ? state->maintenance : world.countries.maintenance[me];
            if (ImGui::SliderInt("Army maintenance", &maintenance, ui.content.economy.maintenance_min, 100, "%d%%")) {
                state->maintenance = maintenance;
            }
            if (ImGui::IsItemActivated()) {
                state->maintenance = maintenance;
                state->maintenance_edited = true;
            }
            if (ImGui::IsItemDeactivated()) {
                if (state->maintenance != world.countries.maintenance[me]) {
                    commands->set_maintenance = state->maintenance;
                }
                state->maintenance_edited = false;
            }
            const Fixed ceiling = morale_ceiling(ui.cache.country_modifiers, world.countries, ui.content.economy, me);
            if (ceiling != kNoMoraleCeiling) {
                ImGui::TextDisabled("  morale up to %.2f", fx_to_double(ceiling));
            }
            ImGui::PopItemWidth();
        }

        // The manpower and the recruitment (game/economy/manpower.h, game/recruitment.h): the pool, a province and a
        // type, "Recruit", the queues.
        void recruitment(const ScreenContext& ui, std::uint16_t me, DevWindowState* state, UiCommands* commands)
        {
            const WorldState& world = ui.state;
            const Content& content = ui.content;
            const std::int32_t maximum = max_manpower(world, content, me);
            ImGui::Text("Manpower %d / %d", world.countries.manpower[me], maximum);
            ImGui::TextDisabled("  +%d a month", monthly_manpower(content.economy, maximum));
            if (content.regiment_types.size() < 2) {
                ImGui::TextDisabled("No regiment types");
                return;
            }

            const Provinces& provinces = world.provinces;
            std::uint16_t& where = state->recruit_province;
            if (where == 0 || where >= provinces.count || provinces.owner[where] != me) {
                where = static_cast<std::uint16_t>(seat(world, me));
            }
            if (where == 0) {
                ImGui::TextDisabled("No provinces");
                return;
            }
            ImGui::PushItemWidth(180.0f * ui.scale);
            const std::string shown(ui.map.province_name(where));
            if (ImGui::BeginCombo("Province", shown.c_str())) {
                for (std::uint32_t id = 1; id < provinces.count; ++id) {
                    if (provinces.owner[id] != me) {
                        continue;
                    }
                    const std::string label = std::string(ui.map.province_name(id)) + "##" + std::to_string(id);
                    if (ImGui::Selectable(label.c_str(), id == where)) {
                        where = static_cast<std::uint16_t>(id);
                    }
                }
                ImGui::EndCombo();
            }
            std::uint8_t& type = state->recruit_type;
            if (type == 0 || type >= content.regiment_types.size()) {
                type = 1;
            }
            if (ImGui::BeginCombo("Type", content.regiment_types.name[type].c_str())) {
                for (std::size_t t = 1; t < content.regiment_types.size(); ++t) {
                    if (ImGui::Selectable(content.regiment_types.name[t].c_str(), t == type)) {
                        type = static_cast<std::uint8_t>(t);
                    }
                }
                ImGui::EndCombo();
            }
            ImGui::PopItemWidth();
            ImGui::TextDisabled("  %.2f emeralds, %d men, %d days", to_emeralds(recruit_cost(world, content, me, type)),
                                kFullRegiment, recruit_time(content, type));
            const char* why = why_no_recruit(world, content, me, where, type);
            ImGui::BeginDisabled(why != nullptr);
            if (ImGui::SmallButton("Recruit")) {
                commands->recruit_province = where;
                commands->recruit_type = type;
            }
            ImGui::EndDisabled();
            if (why != nullptr) {
                ImGui::SameLine();
                ImGui::TextDisabled("(%s)", why);
            }

            // The regiments ordered, by province in slot order: the first one's days left, the others waiting.
            const Recruits& recruits = world.recruits;
            bool any = false;
            for_each_alive(recruits.alive, [&](std::uint32_t r) {
                if (recruits.country[r] != me) {
                    return;
                }
                any = true;
                const std::string province(ui.map.province_name(recruits.province[r]));
                const char* name = content.regiment_types.name[recruits.type[r]].c_str();
                if (recruits.place[r] == 0) {
                    ImGui::BulletText("%s in %s: %d days left%s", name, province.c_str(),
                                      recruit_time(content, recruits.type[r]) - recruits.days[r],
                                      recruitment_paused(world, recruits.province[r], me) ? ", held by a siege" : "");
                } else {
                    ImGui::BulletText("%s in %s: waiting, %d ahead", name, province.c_str(), recruits.place[r]);
                }
            });
            if (!any) {
                ImGui::TextDisabled("Nothing being raised");
            }
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
        ImGui::TextUnformatted("Recruitment");
        recruitment(ui, me, state, commands);

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
