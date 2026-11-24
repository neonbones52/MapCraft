#pragma once

#include <cstdint>

namespace mc
{
    class World;

    // One game from the player's side: the country selection, then playing as the chosen country. Pure state and
    // rules: no rendering, no UI, no camera, no clock (app/shell.h turns its changes into those). Days pass only in
    // the playing phase, at the pace (app/pace.h) the Shell keeps.
    class Session
    {
    public:
        enum class Phase
        {
            choose_country,
            playing,
        };

        // Back to the country selection; the last choice stays selected.
        void new_game();
        // The selection's choice; only a country owning land can be chosen. False (and nothing changes) otherwise.
        bool select(const World& world, std::uint16_t country);
        // Starts as the selected country. False if none is selected or the game already runs.
        bool play();

        [[nodiscard]] Phase phase() const { return phase_; }
        [[nodiscard]] std::uint16_t selected() const { return selected_; }
        [[nodiscard]] std::uint16_t player() const { return player_; }
        [[nodiscard]] bool playing() const { return phase_ == Phase::playing; }

    private:
        Phase phase_ = Phase::choose_country;
        std::uint16_t selected_ = 0;
        std::uint16_t player_ = 0;
    };
}
