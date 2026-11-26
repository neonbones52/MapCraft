#pragma once

#include <cstdint>
#include <filesystem>
#include <span>
#include <utility>

#include <d3d11.h>

#include "view/army_marker.h"
#include "view/input.h"
#include "ui/flags.h"
#include "ui/imgui_layer.h"
#include "ui/screens/screen.h"

namespace mc
{
    class World;

    // The game's UI: the ImGui layer, the flags, and the screen of the session's phase. Reads the game, returns what
    // the player asked for (UiCommands); never changes the game itself.
    class GameUi
    {
    public:
        // `flags` holds <TAG>.png per country, `fonts` the UI's .ttf files (ImGuiLayer).
        bool init(ID3D11Device* device, ID3D11DeviceContext* context, std::filesystem::path flags,
                  const std::filesystem::path& fonts);

        void begin(const Input& input, float view_width, float view_height, float dt);
        [[nodiscard]] bool wants_mouse() const { return ready_ && layer_.wants_mouse(); }
        // `hovered`: the province under the cursor on the map (0 when the UI has the mouse); `armies`: where this
        // frame's army plates go (App::army_markers), drawn under every window.
        UiCommands build(const MapView& map, const World& world, const Session& session, const Pace& pace,
                         std::uint32_t hovered, const ArmyMarkers& armies, const Input& input);
        void render(ID3D11DeviceContext* context, ID3D11RenderTargetView* target);


    private:
        ImGuiLayer layer_;
        FlagCache flags_;
        bool ready_ = false;
        float width_ = 0.0f;
        float height_ = 0.0f;
    };
}
