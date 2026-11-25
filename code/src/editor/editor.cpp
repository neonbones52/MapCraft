#include "editor/editor.h"

#include <algorithm>
#include <format>
#include <span>

#include <imgui.h>
#include <imgui_internal.h>   // ImGuiItemFlags_MixedValue: a checkbox for a field the selection disagrees on

#include "app/app.h"
#include "game/armies.h"
#include "view/map_view.h"

namespace mc
{
    namespace
    {
        constexpr std::size_t kUndoDepth = 64;   // a snapshot is the whole WorldState, ~0.8 MB with the slot tables
        constexpr const char* kMixed = "(mixed)";

        ImVec4 imgui_color(std::uint32_t c)
        {
            return { (c & 255) / 255.0f, ((c >> 8) & 255) / 255.0f, ((c >> 16) & 255) / 255.0f, 1.0f };
        }

        void swatch(std::uint32_t c)
        {
            const float h = ImGui::GetTextLineHeight();
            ImGui::ColorButton("##swatch", imgui_color(c), ImGuiColorEditFlags_NoTooltip | ImGuiColorEditFlags_NoDragDrop,
                               ImVec2(h, h));
            ImGui::SameLine();
        }

        // The value all of `ids` share; false if it is mixed (or there are none).
        template <class T>
        bool shared(const Column<T>& column, std::span<const std::uint32_t> ids, T* value)
        {
            if (ids.empty()) {
                return false;
            }
            *value = column[ids[0]];
            for (const std::uint32_t id : ids) {
                if (column[id] != *value) {
                    return false;
                }
            }
            return true;
        }

        std::string country_label(const CountryDefinitions& countries, std::size_t c)
        {
            return c == 0 ? std::string("-") : std::format("{}  {}", countries.tag_text(c), countries.name[c]);
        }
    }

    Editor::Editor(App& app, GameData& game, std::filesystem::path data_dir)
        : app_(app), game_(game), data_dir_(std::move(data_dir))
    {
        selected_.assign(app_.map().count, 0);
        country_ = game_.content.countries.size() > 1 ? 1 : 0;
    }

    // ------------------------------------------------------------------------------------------------ edits, undo

    void Editor::begin_edit()
    {
        const GameData& game = game_;
        undo_.push_back(game.state);
        if (undo_.size() > kUndoDepth) {
            undo_.pop_front();
        }
        redo_.clear();
    }

    void Editor::begin_typing()
    {
        const ImGuiID item = ImGui::GetItemID();
        if (item != typing_item_) {
            begin_edit();
            typing_item_ = item;
        }
    }

    void Editor::changed()
    {
        dirty_ = true;
        app_.refresh_map_mode();
    }

    void Editor::undo()
    {
        if (undo_.empty()) {
            return;
        }
        GameData& game = game_;
        redo_.push_back(game.state);
        game.state = std::move(undo_.back());
        undo_.pop_back();
        changed();
    }

    void Editor::redo()
    {
        if (redo_.empty()) {
            return;
        }
        GameData& game = game_;
        undo_.push_back(game.state);
        game.state = std::move(redo_.back());
        redo_.pop_back();
        changed();
    }

    bool Editor::save()
    {
        std::string error;
        if (!game_.save_setup(data_dir_, app_.map(), &error)) {
            message_ = "Save failed: " + error;
            return false;
        }
        dirty_ = false;
        message_ = std::format("Saved to {}", (data_dir_ / "setup").string());
        return true;
    }

    // ------------------------------------------------------------------------------------------------ selection

    void Editor::show_selection()
    {
        std::fill(selected_.begin(), selected_.end(), 0);
        for (const std::uint32_t id : selection_) {
            selected_[id] = 1;
        }
        app_.set_selection(selected_);
    }

    void Editor::set_selection(std::vector<std::uint32_t> ids)
    {
        selection_ = std::move(ids);
        show_selection();
    }

    void Editor::select_toggle(std::uint32_t id, bool add_only)
    {
        const auto it = std::find(selection_.begin(), selection_.end(), id);
        if (it == selection_.end()) {
            selection_.push_back(id);
        } else if (!add_only) {
            selection_.erase(it);
        }
        show_selection();
    }

    void Editor::map_input(const Input& input, bool ctrl, bool shift, bool alt)
    {
        const MapView& map = app_.map();
        const std::uint32_t id = app_.hovered();
        const bool land = id != 0 && id < map.count && map.kind[id] == ProvinceKind::land;
        if (app_.map_mode() == MapMode::trade_nodes && !ctrl && !shift && !alt) {
            // One stroke (press, drag, release) is one undo step, taken at its first change.
            if ((input.pressed & mouse_left) != 0) {
                last_painted_region_ = 0;
                stroke_edited_ = false;
            }
            if ((input.buttons & mouse_left) != 0 && land) {
                const std::uint16_t region = map.region[id];
                Regions& regions = game_.state.regions;
                if (region != last_painted_region_ && region < regions.count &&
                    regions.trade_node[region] != paint_node_) {
                    if (!stroke_edited_) {
                        begin_edit();
                        stroke_edited_ = true;
                    }
                    regions.trade_node[region] = static_cast<std::uint8_t>(paint_node_);
                    changed();
                }
                last_painted_region_ = region;
            }
            return;
        }
        if (input.pressed & mouse_left) {
            last_painted_ = 0;
            if (!land) {
                if (!ctrl && !shift) {
                    set_selection({});
                }
                return;
            }
            if (alt) {
                std::vector<std::uint32_t> region = shift || ctrl ? selection_ : std::vector<std::uint32_t>{};
                for (std::uint32_t p = 1; p < map.count; ++p) {
                    if (map.kind[p] == ProvinceKind::land && map.region[p] == map.region[id] &&
                        std::find(region.begin(), region.end(), p) == region.end()) {
                        region.push_back(p);
                    }
                }
                set_selection(std::move(region));
            } else if (ctrl) {
                select_toggle(id, false);
            } else if (shift) {
                select_toggle(id, true);
            } else {
                set_selection({ id });
            }
            last_painted_ = id;
        } else if ((input.buttons & mouse_left) && shift && land && id != last_painted_) {
            select_toggle(id, true);   // Shift+drag paints the selection
            last_painted_ = id;
        }
    }

    void Editor::focus(std::uint32_t id, float, float)
    {
        const MapView& map = app_.map();
        if (id == 0 || id >= map.count) {
            return;
        }
        const MapView::Point at = map.text_position(id);
        app_.camera().set_view(at.x, at.y, std::min(app_.camera().scale(), 1.2f));
    }

    // ------------------------------------------------------------------------------------------------ fields

    void Editor::pick_country(const char* label, Column<std::uint16_t>& column, bool owner)
    {
        GameData& game = game_;
        std::uint16_t value = 0;
        const bool same = shared(column, selection_, &value);
        const std::string preview = same ? country_label(game.content.countries, value) : kMixed;
        if (!ImGui::BeginCombo(label, preview.c_str())) {
            return;
        }
        for (std::size_t c = 0; c < game.content.countries.size(); ++c) {
            ImGui::PushID(static_cast<int>(c));
            if (c != 0) {
                swatch(game.content.countries.color[c]);
            }
            if (ImGui::Selectable(country_label(game.content.countries, c).c_str(), same && value == c)) {
                begin_edit();
                for (const std::uint32_t id : selection_) {
                    column[id] = static_cast<std::uint16_t>(c);
                    if (owner && owner_sets_controller_) {
                        game.state.provinces.controller[id] = static_cast<std::uint16_t>(c);
                    }
                    if (owner && owner_gets_core_ && c != 0) {
                        game.state.provinces.set_core(id, c, true);
                    }
                }
                changed();
            }
            ImGui::PopID();
        }
        ImGui::EndCombo();
    }

    void Editor::pick_listed(const char* label, const DefinitionList& list, Column<std::uint8_t>& column,
                             std::span<const std::uint32_t> ids)
    {
        std::uint8_t value = 0;
        const bool same = shared(column, ids, &value);
        const std::string preview = !same ? kMixed : value == 0 ? "-" : list.name[value];
        ImGui::BeginDisabled(list.size() <= 1);
        if (ImGui::BeginCombo(label, preview.c_str())) {
            for (std::size_t i = 0; i < list.size(); ++i) {
                ImGui::PushID(static_cast<int>(i));
                if (i != 0) {
                    swatch(list.color[i]);
                }
                if (ImGui::Selectable(i == 0 ? "-" : list.name[i].c_str(), same && value == i)) {
                    begin_edit();
                    for (const std::uint32_t id : ids) {
                        column[id] = static_cast<std::uint8_t>(i);
                    }
                    changed();
                }
                ImGui::PopID();
            }
            ImGui::EndCombo();
        }
        ImGui::EndDisabled();
        if (list.size() <= 1) {
            ImGui::SameLine();
            ImGui::TextDisabled("(no list yet)");
        }
    }

    void Editor::edit_number(const char* label, Column<float>& column, std::span<const std::uint32_t> ids, float step,
                             float max)
    {
        float value = 0.0f;
        const bool same = shared(column, ids, &value);
        if (!same) {
            value = 0.0f;
        }
        if (ImGui::InputFloat(label, &value, step, step * 10.0f, "%g")) {
            begin_typing();
            for (const std::uint32_t id : ids) {
                column[id] = std::clamp(value, 0.0f, max);
            }
            changed();
        }
        if (!same) {
            ImGui::SameLine();
            ImGui::TextDisabled("%s", kMixed);   // the field shows 0; what is typed goes to the whole selection
        }
    }

    void Editor::edit_cores()
    {
        GameData& game = game_;
        const CountryDefinitions& countries = game.content.countries;
        if (!ImGui::BeginTable("cores", 3, ImGuiTableFlags_SizingStretchSame)) {
            return;
        }
        for (std::size_t c = 1; c < countries.size(); ++c) {
            std::size_t with = 0;
            for (const std::uint32_t id : selection_) {
                with += game.state.provinces.has_core(id, c);
            }
            ImGui::TableNextColumn();
            ImGui::PushID(static_cast<int>(c));
            bool all = with == selection_.size();
            const bool mixed = with != 0 && !all;
            if (mixed) {
                ImGui::PushItemFlag(ImGuiItemFlags_MixedValue, true);
            }
            if (ImGui::Checkbox(countries.tag_text(c).c_str(), &all)) {
                begin_edit();
                for (const std::uint32_t id : selection_) {
                    game.state.provinces.set_core(id, c, all);
                }
                changed();
            }
            if (mixed) {
                ImGui::PopItemFlag();
            }
            ImGui::SetItemTooltip("%s", countries.name[c].c_str());
            ImGui::PopID();
        }
        ImGui::EndTable();
    }

    // ------------------------------------------------------------------------------------------------ panels

    void Editor::province_panel()
    {
        GameData& game = game_;
        const MapView& map = app_.map();
        ImGui::SetNextWindowPos(ImVec2(10, 10), ImGuiCond_FirstUseEver);
        ImGui::SetNextWindowSize(ImVec2(420, 620), ImGuiCond_FirstUseEver);
        if (!ImGui::Begin("Provinces")) {
            ImGui::End();
            return;
        }
        if (selection_.empty()) {
            ImGui::TextWrapped("Click a province on the map. Ctrl+click toggles, Shift+click or Shift+drag adds, "
                               "Alt+click takes its whole region.");
            ImGui::End();
            return;
        }
        if (selection_.size() == 1) {
            ImGui::Text("%s", std::string(map.province_name(selection_[0])).c_str());
            ImGui::SameLine();
            ImGui::TextDisabled("#%u, %s", selection_[0],
                                std::string(map.region_name(map.region[selection_[0]])).c_str());
        } else {
            ImGui::Text("%zu provinces", selection_.size());
            ImGui::SetItemTooltip("%s", [&] {
                std::string names;
                for (std::size_t i = 0; i < selection_.size() && i < 40; ++i) {
                    names += std::string(map.province_name(selection_[i])) + "\n";
                }
                if (selection_.size() > 40) {
                    names += "...";
                }
                return names;
            }().c_str());
        }
        ImGui::Separator();

        ImGui::SeparatorText("Owner");
        pick_country("Owner", game.state.provinces.owner, true);
        ImGui::Checkbox("Owner also controls", &owner_sets_controller_);
        ImGui::SameLine();
        ImGui::Checkbox("Owner gets a core", &owner_gets_core_);
        pick_country("Controller", game.state.provinces.controller, false);
        if (selection_.size() == 1) {
            const std::uint16_t owner = game.state.provinces.owner[selection_[0]];
            ImGui::BeginDisabled(owner == 0 || game.state.countries.capital[owner] == selection_[0]);
            if (ImGui::Button("Make it the owner's capital")) {
                begin_edit();
                game.state.countries.capital[owner] = static_cast<std::uint16_t>(selection_[0]);
                changed();
            }
            ImGui::EndDisabled();
            if (owner != 0 && game.state.countries.capital[owner] == selection_[0]) {
                ImGui::SameLine();
                ImGui::TextDisabled("(capital)");
            }
        }
        ImGui::SeparatorText("Cores");
        edit_cores();

        ImGui::SeparatorText("Land");
        pick_listed("Terrain", game.content.terrain, game.state.provinces.terrain, selection_);
        ImGui::SeparatorText("Economy");
        pick_listed("Good", game.content.goods, game.state.provinces.good, selection_);
        edit_number("Base tax", game.state.provinces.base_tax, selection_, 1.0f);
        edit_number("Population", game.state.provinces.population, selection_, 1000.0f);

        if (selection_.size() == 1 && map.kind[selection_[0]] == ProvinceKind::land) {
            ImGui::SeparatorText("Armies");
            army_section(selection_[0]);
        }
        ImGui::End();
    }

    void Editor::country_panel(float view_width, float view_height)
    {
        GameData& game = game_;
        const MapView& map = app_.map();
        const CountryDefinitions& countries = game.content.countries;
        CountryStates& states = game.state.countries;
        ImGui::SetNextWindowPos(ImVec2(view_width - 370, 10), ImGuiCond_FirstUseEver);
        ImGui::SetNextWindowSize(ImVec2(360, 300), ImGuiCond_FirstUseEver);
        if (!ImGui::Begin("Countries")) {
            ImGui::End();
            return;
        }
        if (countries.size() <= 1) {
            ImGui::TextDisabled("No countries in data/common/countries.csv");
            ImGui::End();
            return;
        }
        country_ = std::clamp(country_, 1, static_cast<int>(countries.size()) - 1);
        if (ImGui::BeginCombo("Country", country_label(countries, country_).c_str())) {
            for (std::size_t c = 1; c < countries.size(); ++c) {
                ImGui::PushID(static_cast<int>(c));
                swatch(countries.color[c]);
                if (ImGui::Selectable(country_label(countries, c).c_str(), country_ == static_cast<int>(c))) {
                    country_ = static_cast<int>(c);
                }
                ImGui::PopID();
            }
            ImGui::EndCombo();
        }
        std::vector<std::uint32_t> owned;
        for (std::uint32_t id = 1; id < map.count; ++id) {
            if (game.state.provinces.owner[id] == country_) {
                owned.push_back(id);
            }
        }
        ImGui::Text("%zu provinces", owned.size());
        const std::uint16_t capital = states.capital[country_];
        ImGui::Text("Capital: %s", capital ? std::string(map.province_name(capital)).c_str() : "-");
        ImGui::BeginDisabled(owned.empty());
        if (ImGui::Button("Select its provinces")) {
            set_selection(owned);
        }
        ImGui::EndDisabled();
        ImGui::SameLine();
        ImGui::BeginDisabled(capital == 0);
        if (ImGui::Button("Go to capital")) {
            focus(capital, view_width, view_height);
        }
        ImGui::SameLine();
        if (ImGui::Button("Clear capital")) {
            begin_edit();
            states.capital[country_] = 0;
            changed();
        }
        ImGui::EndDisabled();
        ImGui::BeginDisabled(selection_.size() != 1);
        if (ImGui::Button("Capital = the selected province")) {
            begin_edit();
            states.capital[country_] = static_cast<std::uint16_t>(selection_[0]);
            changed();
        }
        ImGui::EndDisabled();
        const std::uint32_t country[] = { static_cast<std::uint32_t>(country_) };
        edit_number("Discipline", states.discipline, country, 0.05f);
        ImGui::SetItemTooltip("1 = 100%%");
        edit_number("Morale", states.morale, country, 0.1f);
        ImGui::SetItemTooltip("The base maximum morale of its regiments");

        const auto warnings = game.check(map);
        if (!warnings.empty()) {
            ImGui::SeparatorText("Warnings");
            for (const std::string& w : warnings) {
                ImGui::TextColored(ImVec4(1.0f, 0.7f, 0.3f, 1.0f), "%s", w.c_str());
            }
        }
        ImGui::End();
    }

    void Editor::map_mode_panel()
    {
        ImGui::SetNextWindowPos(ImVec2(10, 640), ImGuiCond_FirstUseEver);
        if (!ImGui::Begin("Map mode", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
            ImGui::End();
            return;
        }
        for (std::uint32_t m = 0; m < kMapModeCount; ++m) {
            const auto mode = static_cast<MapMode>(m);
            if (ImGui::RadioButton(map_mode_name(mode), app_.map_mode() == mode)) {
                app_.set_map_mode(mode);
            }
            if (m % 2 == 0 && m + 1 < kMapModeCount) {
                ImGui::SameLine(ImGui::GetFontSize() * 10.0f);
            }
        }
        ImGui::End();
    }

    void Editor::trade_node_panel()
    {
        const GameData& game = game_;
        const DefinitionList& nodes = game.content.trade_nodes;
        const MapView& map = app_.map();
        ImGui::SetNextWindowPos(ImVec2(440, 10), ImGuiCond_FirstUseEver);
        if (!ImGui::Begin("Trade nodes", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
            ImGui::End();
            return;
        }
        if (nodes.size() <= 1) {
            ImGui::TextDisabled("No trade nodes in data/common/trade_nodes.csv");
            ImGui::End();
            return;
        }
        paint_node_ = std::clamp(paint_node_, 0, static_cast<int>(nodes.size()) - 1);
        std::vector<std::size_t> regions_of(nodes.size(), 0);
        for (std::uint32_t r = 1; r < game.state.regions.count; ++r) {
            ++regions_of[game.state.regions.trade_node[r]];
        }
        ImGui::TextUnformatted("Click or drag on the map: the regions go to");
        for (std::size_t n = 0; n < nodes.size(); ++n) {
            ImGui::PushID(static_cast<int>(n));
            if (n != 0) {
                swatch(nodes.color[n]);
            }
            const std::string label = std::format("{} ({} regions)", n == 0 ? "none" : nodes.name[n], regions_of[n]);
            if (ImGui::RadioButton(label.c_str(), paint_node_ == static_cast<int>(n))) {
                paint_node_ = static_cast<int>(n);
            }
            ImGui::PopID();
        }
        const std::uint32_t id = app_.hovered();
        if (id != 0 && id < map.count && map.kind[id] == ProvinceKind::land) {
            ImGui::Separator();
            const std::uint16_t region = map.region[id];
            const std::uint8_t n = region < game.state.regions.count ? game.state.regions.trade_node[region] : 0;
            ImGui::TextDisabled("%s: %s", std::string(map.region_name(region)).c_str(),
                                n == 0 ? "no trade node" : nodes.name[n].c_str());
        }
        ImGui::End();
    }

    void Editor::status_panel(float view_width, float view_height)
    {
        const GameData& game = game_;
        const MapView& map = app_.map();
        std::size_t land = 0;
        std::uint32_t first_unfilled = 0;
        std::size_t unfilled = 0;
        const Provinces& p = game.state.provinces;
        for (std::uint32_t id = 1; id < map.count; ++id) {
            if (map.kind[id] != ProvinceKind::land) {
                continue;
            }
            ++land;
            const bool missing = !p.owner[id] || !p.controller[id] || !p.terrain[id] || !p.good[id];
            if (missing) {
                ++unfilled;
                if (first_unfilled == 0) {
                    first_unfilled = id;
                }
            }
        }
        ImGui::SetNextWindowPos(ImVec2(view_width * 0.5f, view_height - 10), ImGuiCond_Always, ImVec2(0.5f, 1.0f));
        ImGui::Begin("##status", nullptr,
                     ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoMove |
                         ImGuiWindowFlags_NoSavedSettings);
        if (ImGui::Button(dirty_ ? "Save*" : "Save")) {
            save();
        }
        ImGui::SameLine();
        ImGui::BeginDisabled(undo_.empty());
        if (ImGui::Button("Undo")) {
            undo();
        }
        ImGui::EndDisabled();
        ImGui::SameLine();
        ImGui::BeginDisabled(redo_.empty());
        if (ImGui::Button("Redo")) {
            redo();
        }
        ImGui::EndDisabled();
        ImGui::SameLine();
        ImGui::Text("| %zu selected | filled %zu of %zu", selection_.size(), land - unfilled, land);
        ImGui::SameLine();
        ImGui::BeginDisabled(first_unfilled == 0);
        if (ImGui::Button("Next unfilled")) {
            set_selection({ first_unfilled });
            focus(first_unfilled, view_width, view_height);
        }
        ImGui::EndDisabled();
        if (!message_.empty()) {
            ImGui::SameLine();
            ImGui::TextDisabled("| %s", message_.c_str());
        }
        ImGui::End();
    }

    void Editor::hover_tooltip()
    {
        const std::uint32_t id = app_.hovered();
        const MapView& map = app_.map();
        if (id == 0 || id >= map.count || ImGui::GetIO().WantCaptureMouse) {
            return;
        }
        ImGui::BeginTooltip();
        ImGui::Text("%s", std::string(map.province_name(id)).c_str());
        if (map.kind[id] == ProvinceKind::land && game_.loaded) {
            const GameData& game = game_;
            const Provinces& p = game.state.provinces;
            ImGui::TextDisabled("%s", std::string(map.region_name(map.region[id])).c_str());
            ImGui::Text("Owner: %s", country_label(game.content.countries, p.owner[id]).c_str());
            ImGui::Text("Terrain: %s", p.terrain[id] ? game.content.terrain.name[p.terrain[id]].c_str() : "-");
            ImGui::Text("Good: %s", p.good[id] ? game.content.goods.name[p.good[id]].c_str() : "-");
            ImGui::Text("Base tax: %g, population: %g", p.base_tax[id], p.population[id]);
            std::size_t armies = 0;
            for_each_alive(game.state.armies.alive,
                           [&](std::uint32_t a) { armies += game.state.armies.province[a] == id; });
            if (armies > 0) {
                ImGui::Text("Armies: %zu", armies);
            }
        }
        ImGui::EndTooltip();
    }

    void Editor::ui(float view_width, float view_height)
    {
        const ImGuiIO& io = ImGui::GetIO();
        if (!ImGui::IsAnyItemActive()) {
            typing_item_ = 0;
        }
        if (!io.WantTextInput) {
            if (ImGui::IsKeyChordPressed(ImGuiMod_Ctrl | ImGuiKey_S)) {
                save();
            }
            if (ImGui::IsKeyChordPressed(ImGuiMod_Ctrl | ImGuiKey_Z)) {
                undo();
            }
            if (ImGui::IsKeyChordPressed(ImGuiMod_Ctrl | ImGuiKey_Y) ||
                ImGui::IsKeyChordPressed(ImGuiMod_Ctrl | ImGuiMod_Shift | ImGuiKey_Z)) {
                redo();
            }
            if (ImGui::IsKeyPressed(ImGuiKey_Escape, false)) {
                set_selection({});
            }
        }
        if (!game_.loaded) {
            ImGui::SetNextWindowPos(ImVec2(view_width * 0.5f, view_height * 0.5f), ImGuiCond_Always,
                                    ImVec2(0.5f, 0.5f));
            ImGui::Begin("No game data", nullptr, ImGuiWindowFlags_AlwaysAutoResize);
            ImGui::Text("%s", message_.c_str());
            ImGui::End();
            return;
        }
        province_panel();
        country_panel(view_width, view_height);
        map_mode_panel();
        if (app_.map_mode() == MapMode::trade_nodes) {
            trade_node_panel();
        }
        status_panel(view_width, view_height);
        hover_tooltip();
    }
}
