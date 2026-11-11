#pragma once

#include <cstdint>
#include <filesystem>
#include <string>

#include <d3d11.h>

#include "app/app.h"
#include "game/session.h"
#include "game/world.h"
#include "ui/game_ui.h"

namespace mc
{
    // The game as its host runs it (mapcraft.exe), wiring layers that do not know each other:
    //   game/World     the game's data and state (owned here, before the App that shows it)
    //   app/App        the map: camera, map modes, picking, drawing; reads the World's data through a pointer
    //   game/Session   the player's side: country selection, then the player's country and the pace of time
    //   ui/GameUi      the screens: read the state, return UiCommands
    // Each frame: input goes to the UI first; keys become intents here (app/bindings.h: camera, map modes, speed, Esc),
    // the map gets only the camera's; Session::advance runs the days the pace lets through on the World; map clicks and
    // UI commands become Session calls; Session changes become map changes (selection highlight, camera flight). The
    // editor uses App alone.
    class Shell
    {
    public:
        // After app().init: the world from data/ (docs/GAMEDATA.md); on an error the map shows the regions.
        bool load_world(const std::filesystem::path& dir, std::string* error);
        // After load_world. `flags` holds <TAG>.png per country.
        void init(ID3D11Device* device, ID3D11DeviceContext* context, std::filesystem::path flags);
        void frame(ID3D11DeviceContext* context, ID3D11RenderTargetView* target, float view_width, float view_height,
                   float dt, const Input& input);

        void new_game();   // the country selection, the whole map in view

        [[nodiscard]] App& app() { return app_; }
        [[nodiscard]] const World& world() const { return world_; }
        [[nodiscard]] const Session& session() const { return session_; }
        // Esc was pressed: the host leaves (the window closes).
        [[nodiscard]] bool exit_requested() const { return exit_requested_; }
        void clear_exit_request() { exit_requested_ = false; }

    private:
        void select(std::uint16_t country);
        void play();
        void highlight();   // the selected country's provinces hatched while choosing

        World world_;     // before app_: the App holds a pointer into it
        App app_;
        Session session_;
        GameUi ui_;
        bool fly_ = false;   // play() happened: fly to the player's seat once the camera is set up
        bool exit_requested_ = false;
    };
}
