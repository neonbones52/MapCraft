#pragma once

#include <filesystem>
#include <span>
#include <string>

#include "game/calendar.h"
#include "game/economy/economy.h"
#include "game/game_data.h"

namespace mc
{
    class MapData;

    // The running game's world, built from data/ at the start. Owns:
    //   Content     what the game is made of (lists, country definitions, strings): fixed once loaded
    //   WorldState  the state (SoA columns): changed only by the World's own rules (the daily tick)
    //   the day     the date, as a day count (game/calendar.h): one number, so it lives here and not among the columns
    // Everyone else reads: the map (App) through a const GameData pointer to colour its modes, the UI and the
    // Session through the const accessors. Lifetime: as long as whatever shows it (Shell declares it before App).
    class World
    {
    public:
        // Errors leave the world unloaded (data().loaded false): the map then shows the regions instead. `patches`:
        // a scenario's changes to the data files (GameData::load), none in the game.
        bool load(const std::filesystem::path& dir, const MapData& map, std::string* error,
                  std::span<const csv::Patch> patches = {});

        // `days` days pass, one after another: the only way the state and the date change (the game's Shell, at the
        // pace the player set, view/pace.h; mapcraft_sim). Sets the simulation's floating-point environment
        // first (core/det_math.h). Each day runs the rules over the columns (game/economy/economy.h), then checks the
        // money invariant (game/economy/ledger.h) and logs an error the first time it breaks.
        void advance_days(std::uint32_t days);
        // Back to the start: the start date and the state as loaded.
        void restart();

        [[nodiscard]] bool loaded() const { return data_.loaded; }
        [[nodiscard]] const Content& content() const { return data_.content; }
        [[nodiscard]] const WorldState& state() const { return data_.state; }
        // Both, for the map modes (App::set_game).
        [[nodiscard]] const GameData& data() const { return data_; }
        [[nodiscard]] std::uint32_t day() const { return day_; }
        [[nodiscard]] Date date() const { return date_of(day_); }
        // False once the money invariant has broken since the load or the last restart (mapcraft_sim's exit code).
        [[nodiscard]] bool money_balanced() const { return !unbalanced_; }

    private:
        void tick_day();   // one day of the rules

        GameData data_;
        WorldState start_;              // the state as loaded, for restart
        EconomyCache economy_;          // the day's index and scratch (not state: rebuilt on load and restart)
        std::uint32_t day_ = kStartDay;
        bool unbalanced_ = false;       // the money invariant broke (logged once)
    };
}
