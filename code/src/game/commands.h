#pragma once

#include <cstdint>
#include <vector>

#include "game/armies.h"
#include "game/game_data.h"

namespace mc
{
    // What a player (or, later, a bot) asks of the world (docs/SIMULATION.md, "Очередь команд"): plain data, no
    // pointers; an army is named by its slot and that slot's generation (ArmyRef), so an order to an army that died
    // goes to no one, even after another army took its slot.
    enum class CommandKind : std::uint8_t
    {
        none = 0,
        move_army = 1,     // the army marches to `province` (game/movement.h)
        declare_war = 2,   // the giver declares war on `target` (game/war.h)
    };

    struct Command
    {
        CommandKind kind = CommandKind::none;
        std::uint16_t country = 0;   // who gives it: it must own the army
        ArmyRef army;
        std::uint16_t province = 0;
        std::uint16_t target = 0;    // a country: the one war is declared on
    };

    // The one queue of commands of the game and mapcraft_sim (and later of the network): each command gets its place
    // on the time line, (day, number), the number counting the posts in their order (in the network the host will give
    // it). World runs the due ones between days, in that order (World::run_commands). Columns, one per field of
    // Command, in post order. Not state: a command is executed or still due; the single game runs its commands the
    // moment they are posted, mapcraft_sim posts each day's on its day, so between days the queue is empty.
    class CommandQueue
    {
    public:
        // Queues `command` for `day` (today or later).
        void post(std::uint32_t day, const Command& command);
        // Calls run(command) for every command due by `today` (day <= today) in (day, number) order, then drops them.
        template <class Run>
        void run_due(std::uint32_t today, Run&& run)
        {
            due_.clear();
            for (std::uint32_t i = 0; i < day_.size(); ++i) {
                if (day_[i] <= today) {
                    due_.push_back(i);
                }
            }
            if (due_.empty()) {
                return;
            }
            sort_due();
            for (const std::uint32_t i : due_) {
                run(Command{ kind_[i], country_[i], ArmyRef{ army_slot_[i], army_generation_[i] }, province_[i],
                             target_[i] });
            }
            drop(today);
        }
        [[nodiscard]] bool empty() const { return day_.empty(); }

    private:
        void sort_due();                    // due_ by (day, number)
        void drop(std::uint32_t today);     // keeps the commands after today, in order

        Column<std::uint32_t> day_;
        Column<std::uint32_t> number_;
        Column<CommandKind> kind_;
        Column<std::uint16_t> country_;
        Column<std::uint16_t> army_slot_;
        Column<std::uint16_t> army_generation_;
        Column<std::uint16_t> province_;
        Column<std::uint16_t> target_;
        std::uint32_t next_number_ = 0;
        Column<std::uint32_t> due_;   // working: the due commands' rows (reserved by post)
    };
}
