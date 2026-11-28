#include "ui/windows.h"

#include <algorithm>

namespace mc
{
    namespace
    {
        // In GameWindow's order.
        constexpr WindowZone kZones[] = {
            WindowZone::left,   // country
        };
        static_assert(std::size(kZones) == static_cast<std::size_t>(GameWindow::count), "a zone per window");

        WindowZone zone_of(GameWindow window)
        {
            return kZones[static_cast<std::size_t>(window)];
        }
    }

    void Windows::open(GameWindow window)
    {
        for (std::size_t w = 0; w < open_.size(); ++w) {
            const auto other = static_cast<GameWindow>(w);
            if (other != window && open_[w] && zone_of(other) == zone_of(window)) {
                close(other);
            }
        }
        open_[index(window)] = true;
        std::erase(order_, window);
        order_.push_back(window);
    }

    void Windows::close(GameWindow window)
    {
        open_[index(window)] = false;
        std::erase(order_, window);
    }

    void Windows::toggle(GameWindow window)
    {
        if (is_open(window)) {
            close(window);
        } else {
            open(window);
        }
    }

    void Windows::close_all()
    {
        open_.fill(false);
        order_.clear();
    }

    bool Windows::close_top()
    {
        sync();
        if (order_.empty()) {
            return false;
        }
        close(order_.back());
        return true;
    }

    void Windows::sync()
    {
        std::erase_if(order_, [&](GameWindow w) { return !open_[index(w)]; });
    }
}
