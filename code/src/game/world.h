#pragma once

#include <filesystem>
#include <span>
#include <string>

#include "game/calendar.h"
#include "game/combat.h"
#include "game/commands.h"
#include "game/game_data.h"
#include "game/movement.h"
#include "game/occupation.h"
#include "game/recovery.h"

namespace mc
{
    class MapData;

    // The running game's world, built from data/ at the start. Owns:
    //   Content     what the game is made of (lists, country definitions, strings): fixed once loaded
    //   WorldState  the state (SoA columns): changed only by the World's own rules (the daily and monthly ticks) and
    //               the commands' effects
    //   the day     the date, as a day count (game/calendar.h): one number, so it lives here and not among the columns
    //   the seed    the world's randomness (core/random.h): chosen when a game begins (the host's, then the save's)
    //   the rules   their cache (the modifier sums in GameData, the armies' march, game/movement.h, the battles,
    //               game/combat.h, the sieges, game/occupation.h, the recovery, game/recovery.h), rebuilt after a
    //               load and a restart
    //   commands    the queue (game/commands.h): what the players ask, run between days
    // Everyone else reads: the map (App) through a const GameData pointer to colour its modes, the UI and the
    // Session through the const accessors. Lifetime: as long as whatever shows it (Shell declares it before App).
    class World
    {
    public:
        // Errors leave the world unloaded (data().loaded false): the map then shows the regions instead. `seed`: the
        // world's randomness (the game draws one per game, mapcraft_sim takes the scenario's). `patches`: a
        // scenario's changes to the data files (GameData::load), none in the game.
        bool load(const std::filesystem::path& dir, const MapData& map, std::uint32_t seed, std::string* error,
                  std::span<const csv::Patch> patches = {});

        // `days` days pass, one after another: the only way the date changes (the game's Shell, at the pace the
        // player set, view/pace.h; mapcraft_sim). Sets the simulation's floating-point environment first
        // (core/det_math.h). Each day first runs the commands due (run_commands), then the day's rules over the
        // columns: the armies' march, then the battles, then the sieges; a day that ends a month (the next is the
        // 1st) then runs the month's rules (tick_month).
        void advance_days(std::uint32_t days);
        // Queues a command for today (game/commands.h); it runs at run_commands or before the next day.
        void post(const Command& command) { commands_.post(day_, command); }
        // Runs the commands due today, in their order: the other way the state changes, between days and in the
        // pause (the game calls it right after posting, so the player's orders take effect at once). A command that
        // cannot be carried out (an army that is gone or not the giver's, in a battle or retreating from one, no way
        // there; a war that cannot be declared, game/war.h; a peace that cannot be offered, game/peace.h) changes
        // nothing; it is logged. A peace offered is accepted or refused at once (game/peace.h), and logged.
        void run_commands();
        // Back to the start: the start date and the state as loaded, with a new seed (a new game).
        void restart(std::uint32_t seed);

        [[nodiscard]] bool loaded() const { return data_.loaded; }
        [[nodiscard]] const Content& content() const { return data_.content; }
        [[nodiscard]] const WorldState& state() const { return data_.state; }
        // The cache rebuilt from both (the countries' modifier sums, game/modifiers.h).
        [[nodiscard]] const Cache& cache() const { return data_.cache; }
        // All three, for the map modes (App::set_game).
        [[nodiscard]] const GameData& data() const { return data_; }
        [[nodiscard]] std::uint32_t day() const { return day_; }
        [[nodiscard]] std::uint32_t seed() const { return seed_; }
        [[nodiscard]] Date date() const { return date_of(day_); }
        // The march's cache, for the map's figures (where on its step an army is).
        [[nodiscard]] const Movement& movement() const { return movement_; }

    private:
        void tick_day();     // one day of the rules
        // The monthly tick, at the start of each month's 1st day (never at the game's start): the main one of the
        // rules that are not about moving and fighting. Now the armies' recovery, the garrisons' growth and the
        // truces that ran out (game/war.h); later
        // the money, the economy and the sliders (M8, M9). A yearly tick (the census tax, EU3's) comes with the
        // treasury.
        void tick_month();
        void apply(const Command& command);

        GameData data_;
        WorldState start_;              // the state as loaded, for restart
        std::uint32_t day_ = kStartDay;
        std::uint32_t seed_ = 0;
        Movement movement_;
        Combat combat_;
        Occupation occupation_;
        Recovery recovery_;
        CommandQueue commands_;
    };
}
