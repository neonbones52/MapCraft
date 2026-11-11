#include "game/session.h"

#include "game/countries.h"
#include "game/world.h"

namespace mc
{
    void Session::new_game()
    {
        phase_ = Phase::choose_country;
        player_ = 0;
        pace_ = Pace{};
    }

    bool Session::select(const World& world, std::uint16_t country)
    {
        if (phase_ != Phase::choose_country || !owns_land(world.state(), country)) {
            return false;
        }
        selected_ = country;
        return true;
    }

    bool Session::play()
    {
        if (phase_ != Phase::choose_country || selected_ == 0) {
            return false;
        }
        player_ = selected_;
        phase_ = Phase::playing;
        return true;
    }

    void Session::set_speed(int speed)
    {
        if (phase_ == Phase::playing) {
            pace_.set_speed(speed);
        }
    }

    void Session::change_speed(int step)
    {
        if (phase_ == Phase::playing) {
            pace_.change_speed(step);
        }
    }

    void Session::toggle_pause()
    {
        if (phase_ == Phase::playing) {
            pace_.toggle_pause();
        }
    }

    int Session::advance(World& world, float dt)
    {
        if (phase_ != Phase::playing) {
            return 0;
        }
        return pace_.advance(dt, [&world] { world.tick_day(); });
    }
}
