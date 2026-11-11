#pragma once

#include <cstdint>
#include <deque>
#include <filesystem>
#include <string>
#include <vector>

#include "game/game_data.h"
#include "core/input.h"

namespace mc
{
    class App;

    // The province editor over the map (mapcraft_editor.exe): select provinces on the map, set their fields in ImGui
    // panels, undo, save to data/setup (docs/GAMEDATA.md).
    //   click: select one; Ctrl+click: toggle; Shift+click or Shift+drag: add; Alt+click: the whole region
    //   Ctrl+S save, Ctrl+Z undo, Ctrl+Y / Ctrl+Shift+Z redo, Esc clear the selection
    class Editor
    {
    public:
        // `game`: the start being edited, owned by the caller (and shown by `app`, see App::set_game).
        Editor(App& app, GameData& game, std::filesystem::path data_dir);

        // Mouse on the map this frame (not over a panel): selection clicks and drags.
        void map_input(const Input& input, bool ctrl, bool shift, bool alt);
        // The panels and shortcuts; call between ImGui::NewFrame and ImGui::Render.
        void ui(float view_width, float view_height);

        [[nodiscard]] bool dirty() const { return dirty_; }
        void set_message(std::string message) { message_ = std::move(message); }
        bool save();

    private:
        using Snapshot = WorldState;   // undo keeps the whole start state (content never changes here)

        void begin_edit();          // before a change: remember the state for undo
        void begin_typing();        // the same, once per text field activation (typing is one undo step)
        void changed();             // after a change: redraw the map mode, mark unsaved
        void undo();
        void redo();
        void set_selection(std::vector<std::uint32_t> ids);
        void select_toggle(std::uint32_t id, bool add_only);
        void show_selection();      // pushes the flags to the renderer
        void focus(std::uint32_t id, float view_width, float view_height);

        void province_panel();
        void country_panel(float view_width, float view_height);
        void map_mode_panel();
        void status_panel(float view_width, float view_height);
        void hover_tooltip();

        // Field editing over the whole selection.
        void pick_country(const char* label, Column<std::uint16_t>& column, bool owner);
        void pick_listed(const char* label, const DefinitionList& list, Column<std::uint8_t>& column);
        // A country's own field (the country panel's country).
        void pick_country_listed(const char* label, const DefinitionList& list, Column<std::uint8_t>& column);
        void edit_amount(const char* label, Column<float>& column);
        void edit_cores();

        App& app_;
        GameData& game_;
        std::filesystem::path data_dir_;
        std::vector<std::uint32_t> selection_;     // province ids, in the order selected
        std::vector<std::uint8_t> selected_;       // per province id
        std::deque<Snapshot> undo_;
        std::deque<Snapshot> redo_;
        bool dirty_ = false;
        bool owner_sets_controller_ = true;
        bool owner_gets_core_ = true;
        int country_ = 1;                          // country panel's country
        std::string message_;                      // last save / load message
        std::uint32_t last_painted_ = 0;
        unsigned typing_item_ = 0;                 // ImGui id of the field being typed into, 0 = none
    };
}
