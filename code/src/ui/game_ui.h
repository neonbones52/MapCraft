#pragma once

#include <cstdint>
#include <filesystem>
#include <utility>

#include <d3d11.h>

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
        bool init(ID3D11Device* device, ID3D11DeviceContext* context, std::filesystem::path flags);

        void begin(const Input& input, float view_width, float view_height, float dt);
        [[nodiscard]] bool wants_mouse() const { return ready_ && layer_.wants_mouse(); }
        // `hovered`: the province under the cursor on the map (0 when the UI has the mouse).
        // `price_good`: the good the Prices map mode shows, 0 when another mode is on.
        UiCommands build(const MapData& map, const World& world, const Session& session, const Pace& pace,
                         std::uint32_t hovered, std::uint8_t price_good, const Input& input);
        void render(ID3D11DeviceContext* context, ID3D11RenderTargetView* target);


    private:
        ImGuiLayer layer_;
        FlagCache flags_;
        bool ready_ = false;
        float width_ = 0.0f;
        float height_ = 0.0f;
    };
}
