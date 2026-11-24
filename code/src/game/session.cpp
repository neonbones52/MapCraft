#include "game/session.h"

#include "game/countries.h"
#include "game/world.h"

namespace mc
{
    void Session::new_game()
    {
        phase_ = Phase::choose_country;
        player_ = 0;
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
}
