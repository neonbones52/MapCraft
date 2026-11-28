// The Editor's armies: the armies of the selected land province and their regiments, put down, removed and edited
// (editor/editor.h). Slots are taken and freed only through game/armies.h.

#include "editor/editor.h"

#include <format>
#include <string>
#include <utility>
#include <vector>

#include <imgui.h>

#include "game/armies.h"
#include "game/modifiers.h"
#include "ui/army_name.h"

namespace mc
{
    void Editor::army_section(std::uint32_t province)
    {
        GameData& game = game_;
        WorldState& state = game.state;
        const DefinitionList& types = game.content.regiment_types;
        const std::uint16_t owner = state.provinces.owner[province];

        std::vector<std::uint16_t> here;
        for_each_alive(state.armies.alive, [&](std::uint32_t a) {
            if (state.armies.province[a] == province) {
                here.push_back(static_cast<std::uint16_t>(a));
            }
        });

        for (const std::uint16_t army : here) {
            ImGui::PushID(army);
            const std::uint16_t army_owner = state.armies.owner[army];
            const ArmyTotals totals = army_totals(state.regiments, army);
            ImGui::Text("%s (%s): %u regiments, %lld men", ui::army_name(army).c_str(),
                        army_owner ? game.content.countries.tag_text(army_owner).c_str() : "no owner",
                        totals.regiments, static_cast<long long>(totals.men));
            ImGui::SameLine();
            if (ImGui::SmallButton("Remove army")) {
                begin_edit();
                if (state.armies.leader[army] != 0) {
                    destroy_leader(state, static_cast<std::uint16_t>(state.armies.leader[army] - 1u));
                }
                destroy_army(state, army);
                changed();
                ImGui::PopID();
                break;   // `here` is stale now; the next frame lists them again
            }

            // Its general: added (of the army's country), its pips edited, removed with x; an army removed here takes
            // its general with it (a general that outlives its army is the game's, not the start's).
            const std::uint16_t leader = state.armies.leader[army];
            const int most = game.content.combat.leader_max_pips;
            if (leader == 0) {
                ImGui::BeginDisabled(army_owner == 0);
                if (ImGui::SmallButton("Add general")) {
                    std::uint16_t slot = 0;
                    begin_edit();
                    if (create_leader(state.leaders, army_owner, &slot)) {
                        state.armies.leader[army] = static_cast<std::uint16_t>(slot + 1);
                        changed();
                    } else {
                        message_ = std::format("Every one of the {} leader slots is taken", kLeaderSlots);
                    }
                }
                ImGui::EndDisabled();
            } else {
                const std::uint32_t one[] = { leader - 1u };
                ImGui::TextUnformatted("General");
                for (auto [label, column] : { std::pair{ "fire", &state.leaders.fire },
                                              std::pair{ "shock", &state.leaders.shock },
                                              std::pair{ "maneuver", &state.leaders.maneuver },
                                              std::pair{ "siege", &state.leaders.siege } }) {
                    ImGui::SameLine();
                    ImGui::SetNextItemWidth(ImGui::GetFontSize() * 6.0f);
                    edit_whole(label, *column, one, 1, most);
                }
                ImGui::SameLine();
                if (ImGui::SmallButton("x##general")) {
                    begin_edit();
                    destroy_leader(state, static_cast<std::uint16_t>(leader - 1u));
                    changed();
                }
                ImGui::SetItemTooltip("Remove the general");
            }

            std::vector<std::uint16_t> regiments;
            for_each_alive(state.regiments.alive, [&](std::uint32_t r) {
                if (state.regiments.army[r] == army) {
                    regiments.push_back(static_cast<std::uint16_t>(r));
                }
            });
            bool removed = false;
            for (const std::uint16_t r : regiments) {
                ImGui::PushID(r);
                const std::uint32_t one[] = { r };
                ImGui::SetNextItemWidth(ImGui::GetFontSize() * 9.0f);
                pick_listed("##type", types, state.regiments.type, one);
                ImGui::SameLine();
                ImGui::SetNextItemWidth(ImGui::GetFontSize() * 9.0f);
                edit_whole("men", state.regiments.strength, one, 100, kFullRegiment);
                ImGui::SameLine();
                if (ImGui::SmallButton("x")) {
                    begin_edit();
                    remove_regiment(state.regiments, r);
                    changed();
                    removed = true;
                }
                ImGui::SetItemTooltip("Remove the regiment");
                ImGui::PopID();
                if (removed) {
                    break;
                }
            }
            ImGui::BeginDisabled(types.size() <= 1);
            if (ImGui::SmallButton("Add regiment")) {
                std::uint16_t slot = 0;
                begin_edit();
                if (add_regiment(state.regiments, army, 1, kFullRegiment,
                                 max_morale(game.cache.country_modifiers, army_owner), &slot)) {
                    changed();
                } else {
                    message_ = std::format("Every one of the {} regiment slots is taken", kRegimentSlots);
                }
            }
            ImGui::EndDisabled();
            ImGui::Separator();
            ImGui::PopID();
            if (removed) {
                break;
            }
        }

        // A new army of the province's owner, with one full regiment of the first type.
        ImGui::BeginDisabled(owner == 0 || types.size() <= 1);
        if (ImGui::Button("Add army")) {
            std::uint16_t army = 0;
            std::uint16_t regiment = 0;
            begin_edit();
            if (!create_army(state.armies, owner, static_cast<std::uint16_t>(province), &army)) {
                message_ = std::format("Every one of the {} army slots is taken", kArmySlots);
            } else if (!add_regiment(state.regiments, army, 1, kFullRegiment,
                                     max_morale(game.cache.country_modifiers, owner), &regiment)) {
                destroy_army(state, army);
                message_ = std::format("Every one of the {} regiment slots is taken", kRegimentSlots);
            } else {
                changed();
            }
        }
        ImGui::EndDisabled();
        if (owner == 0) {
            ImGui::SameLine();
            ImGui::TextDisabled("(the province has no owner)");
        } else if (types.size() <= 1) {
            ImGui::SameLine();
            ImGui::TextDisabled("(no types in data/common/regiment_types.csv)");
        }
    }
}
