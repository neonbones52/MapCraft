#include "ui/game_ui.h"

#include <algorithm>
#include <cmath>

#include "core/log.h"
#include "game/session.h"
#include "game/world.h"
#include "ui/screens/army_plates.h"
#include "ui/screens/country_select.h"
#include "ui/screens/dev_window.h"
#include "ui/screens/date_bar.h"
#include "ui/screens/province_tooltip.h"
#include "ui/mc_widgets.h"
#include "ui/screens/top_bar.h"
#include "ui/screens/widget_showcase.h"

namespace mc
{
    bool GameUi::init(ID3D11Device* device, ID3D11DeviceContext* context, std::filesystem::path flags,
                      const std::filesystem::path& fonts, const std::filesystem::path& jar)
    {
        ready_ = layer_.init(device, context, fonts);
        if (!ready_) {
            log::error("ImGui DX11 backend failed: the game runs without its UI");
        }
        flags_.init(device, std::move(flags));
        sprites_.load(device, jar);
        banners_.init(&sprites_);
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

    UiCommands GameUi::build(const MapView& map, const World& world, const Session& session, const Pace& pace,
                             std::uint32_t hovered, const ArmyMarkers& armies, const Input& input)
    {
        UiCommands commands;
        plate_ = -1;
        if (!ready_) {
            return commands;
        }
        const ScreenContext ui{ map,     world.content(), world.state(), world.cache(), world.loaded(),
                                session, pace,            flags_,        width_,        height_,
                                layer_.scale(),           world.date(),  layer_.bold() };
        // The GUI scale of the Minecraft widgets (ui/mc_widgets.h): whole, the window's height / 540 rounded (2 from
        // 810 to 1349 px), unless the showcase tries another.
        const int gui_scale = dev_.showcase.gui_scale > 0
                                  ? dev_.showcase.gui_scale
                                  : std::max(1, static_cast<int>(std::lround(height_ / 540.0f)));
        mcui::begin_frame(&sprites_, gui_scale);
        plate_ = army_plates(ui, armies, input.cursor_x, input.cursor_y);
        switch (session.phase()) {
        case Session::Phase::choose_country:
            windows_.close_all();
            top_bar(ui, &banners_, &windows_);
            country_select_screen(ui, &commands);
            province_tooltip(ui, hovered, input.cursor_x, input.cursor_y);
            break;
        case Session::Phase::playing:
            top_bar(ui, &banners_, &windows_);
            date_bar(ui, &commands);
            country_window(ui, &windows_, &country_, &commands);
            if (dev_open_) {
                dev_window(ui, &dev_, &commands, &dev_open_);
            }
            showcase_panel(&dev_.showcase);
            province_tooltip(ui, hovered, input.cursor_x, input.cursor_y);
            break;
        }
        windows_.sync();
        return commands;
    }

    void GameUi::render(ID3D11DeviceContext* context, ID3D11RenderTargetView* target)
    {
        if (ready_) {
            layer_.render(context, target);
        }
    }
}
