#include "app/shell.h"

#include <chrono>
#include <vector>

#include "app/bindings.h"
#include "core/log.h"
#include "game/countries.h"

namespace mc
{
    namespace
    {
        // Play's zoom, counted from the closest: two or three notches out from where the province names take over.
        constexpr int kPlayZoomLevel = 7;
    }

    bool Shell::load_world(const std::filesystem::path& dir, std::string* error)
    {
        const auto started = std::chrono::steady_clock::now();
        const bool loaded = world_.load(dir, app_.map(), error);
        log::info("world load: {:.0f} ms",
                  std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - started).count());
        app_.set_game(&world_.data());
        return loaded;
    }

    void Shell::init(ID3D11Device* device, ID3D11DeviceContext* context, std::filesystem::path flags)
    {
        ui_.init(device, context, std::move(flags));
        new_game();
    }

    void Shell::new_game()
    {
        session_.new_game();
        pace_ = Pace{};   // paused, speed 3
        world_.restart();
        fly_ = false;
        app_.request_camera_reset();
        app_.set_map_mode(MapMode::political);
        highlight();
    }

    void Shell::select(std::uint16_t country)
    {
        if (session_.select(world_, country)) {
            highlight();
        }
    }

    void Shell::play()
    {
        if (!session_.play()) {
            return;
        }
        fly_ = true;
        highlight();
        const CountryDefinitions& countries = world_.content().countries;
        log::info("playing as {} ({})", countries.tag_text(session_.player()), countries.name[session_.player()]);
    }

    void Shell::set_speed(int speed)
    {
        if (session_.playing()) {
            pace_.set_speed(speed);
        }
    }

    void Shell::change_speed(int step)
    {
        if (session_.playing()) {
            pace_.change_speed(step);
        }
    }

    void Shell::toggle_pause()
    {
        if (session_.playing()) {
            pace_.toggle_pause();
        }
    }

    void Shell::highlight()
    {
        const Provinces& provinces = world_.state().provinces;
        std::vector<std::uint8_t> flags(app_.map().count, 0);
        const std::uint16_t c = session_.selected();
        if (session_.phase() == Session::Phase::choose_country && c != 0) {
            for (std::uint32_t id = 1; id < provinces.count; ++id) {
                flags[id] = provinces.owner[id] == c ? 1 : 0;
            }
        }
        app_.set_selection(std::move(flags));
    }

    void Shell::frame(ID3D11DeviceContext* context, ID3D11RenderTargetView* target, float view_width, float view_height,
                      float dt, const Input& input)
    {
        ui_.begin(input, view_width, view_height, dt);
        Input map_input = input;
        if (ui_.wants_mouse()) {
            map_input.cursor_x = map_input.cursor_y = -1.0f;
            map_input.wheel = 0.0f;
            map_input.pressed = 0;
            map_input.buttons &= ~mouse_left;
        }
        if (leave_key(input)) {
            exit_requested_ = true;
        }
        if (const auto mode = map_mode_key(input)) {
            app_.set_map_mode(*mode);
        }
        const SpeedInput speed = speed_input(input);
        if (speed.step != 0) {
            change_speed(speed.step);
        }
        if (speed.pause) {
            toggle_pause();
        }
        // Before the map and the UI draw: they show today's state.
        if (session_.playing() && pace_.advance(dt, [this](std::uint32_t days) { world_.advance_days(days); }) > 0 &&
            app_.map_mode() == MapMode::prices) {
            app_.refresh_colors();   // the prices moved
        }
        if (fly_) {
            // After any pending camera reset, or the reset would cancel the flight.
            app_.ensure_camera(view_width, view_height);
            const std::uint32_t id = seat(world_.state(), session_.player());
            if (id != 0) {
                const MapData::Point city = app_.map().city_position(id);
                app_.camera().fly_to(city.x, city.y, Camera::scale_at_level_from_closest(kPlayZoomLevel), view_width,
                                     view_height);
            }
            fly_ = false;
        }
        app_.frame(context, target, view_width, view_height, dt, camera_input(map_input));

        if (session_.phase() == Session::Phase::choose_country && (map_input.pressed & mouse_left) &&
            app_.hovered() != 0 && world_.loaded()) {
            select(world_.state().provinces.owner[app_.hovered()]);
        }

        const std::uint8_t price_good = app_.map_mode() == MapMode::prices ? app_.price_good() : 0;
        const UiCommands commands = ui_.build(app_.map(), world_, session_, pace_, app_.hovered(), price_good, input);
        ui_.render(context, target);
        if (commands.play) {
            play();
        }
        if (commands.set_speed != 0) {
            set_speed(commands.set_speed);
        }
        if (commands.toggle_pause) {
            toggle_pause();
        }
        if (commands.show_prices > 0) {
            app_.set_price_good(static_cast<std::uint8_t>(commands.show_prices));
            app_.set_map_mode(MapMode::prices);
        } else if (commands.show_prices == 0) {
            app_.set_map_mode(MapMode::political);
        }
        if (commands.new_game) {
            new_game();
        }
    }
}
