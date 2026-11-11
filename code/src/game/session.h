#pragma once

#include <cstdint>

#include "game/pace.h"

namespace mc
{
    class World;

    // One game from the player's side: the country selection, then playing as the chosen country, at the pace the
    // player set. Pure state and rules: no rendering, no UI, no camera (app/shell.h turns its changes into those).
    // The simulation hangs off the playing phase: advance() is what drives the World's daily tick.
    class Session
    {
    public:
        enum class Phase
        {
            choose_country,
            playing,
        };

        // Back to the country selection (a new pace too: paused, speed 3); the last choice stays selected.
        void new_game();
        // The selection's choice; only a country owning land can be chosen. False (and nothing changes) otherwise.
        bool select(const World& world, std::uint16_t country);
        // Starts as the selected country. False if none is selected or the game already runs.
        bool play();

        // The player's time controls (game/pace.h); they do nothing outside the playing phase.
        void set_speed(int speed);
        void change_speed(int step);
        void toggle_pause();
        // One frame of `dt` seconds: the World's days the pace lets through, each a World::tick_day. Nothing outside
        // the playing phase. Returns the days that passed. The World is only borrowed for the call.
        int advance(World& world, float dt);

        [[nodiscard]] Phase phase() const { return phase_; }
        [[nodiscard]] std::uint16_t selected() const { return selected_; }
        [[nodiscard]] std::uint16_t player() const { return player_; }
        [[nodiscard]] const Pace& pace() const { return pace_; }

    private:
        Phase phase_ = Phase::choose_country;
        std::uint16_t selected_ = 0;
        std::uint16_t player_ = 0;
        Pace pace_;
    };
}
