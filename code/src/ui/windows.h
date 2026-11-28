#pragma once

#include <array>
#include <cstdint>
#include <vector>

namespace mc
{
    // The game's windows (docs/UI.md, "Окна"): which are open and in what order, UI state only. Each window has its
    // place on the screen, a zone; opening one closes the other of its zone (EU4: one window on the left at a time).
    // Esc closes the one opened last. New windows: a line in GameWindow and in kZones (windows.cpp).
    enum class GameWindow : std::uint8_t
    {
        country,   // the left: the player's country, by the banner
        count,
    };

    enum class WindowZone : std::uint8_t
    {
        left,          // under the banner, the height of the screen (EU4's country window)
        bottom_left,   // the province or the army picked on the map (later)
        centre,        // the big ones (later)
    };

    class Windows
    {
    public:
        void open(GameWindow window);
        void close(GameWindow window);
        void toggle(GameWindow window);
        void close_all();
        // Esc: closes the window opened last; false if none was open.
        bool close_top();
        [[nodiscard]] bool is_open(GameWindow window) const { return open_[index(window)]; }
        // The flag a window's cross clears (mcui::begin_panel): sync() then takes the window out of the order.
        [[nodiscard]] bool* flag(GameWindow window) { return &open_[index(window)]; }
        void sync();

    private:
        static std::size_t index(GameWindow window) { return static_cast<std::size_t>(window); }

        std::array<bool, static_cast<std::size_t>(GameWindow::count)> open_{};
        std::vector<GameWindow> order_;   // the open ones, the last opened last
    };
}
