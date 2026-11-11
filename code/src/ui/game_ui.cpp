#include "ui/game_ui.h"

#include "core/log.h"
#include "game/session.h"
#include "game/world.h"
#include "ui/screens/country_select.h"
#include "ui/screens/player_hud.h"
#include "ui/screens/time_bar.h"

namespace mc
{
    bool GameUi::init(ID3D11Device* device, ID3D11DeviceContext* context, std::filesystem::path flags)
    {
        ready_ = layer_.init(device, context);
        if (!ready_) {
            log::error("ImGui DX11 backend failed: the game runs without its UI");
        }
        flags_.init(device, std::move(flags));
        return ready_;
    }

    void GameUi::begin(const Input& input, float view_width, float view_height, float dt)
    {
        width_ = view_width;
        height_ = view_height;
        if (ready_) {
            layer_.begin(input, view_width, view_height, dt);
        }
    }

    UiCommands GameUi::build(const MapData& map, const World& world, const Session& session, std::uint32_t hovered,
                             const Input& input)
    {
        UiCommands commands;
        if (!ready_) {
            return commands;
        }
        const ScreenContext ui{ map, world.content(), world.state(), world.loaded(), session, flags_, width_,
                                height_, layer_.scale(), world.date() };
        switch (session.phase()) {
        case Session::Phase::choose_country:
            country_select_screen(ui, &commands);
            country_tooltip(ui, hovered, input.cursor_x, input.cursor_y);
            break;
        case Session::Phase::playing:
            player_hud(ui, &commands);
            time_bar(ui, &commands);
            break;
        }
        return commands;
    }

    void GameUi::render(ID3D11DeviceContext* context, ID3D11RenderTargetView* target)
    {
        if (ready_) {
            layer_.render(context, target);
        }
    }
}
