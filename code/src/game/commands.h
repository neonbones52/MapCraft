#pragma once

#include <cstdint>
#include <span>
#include <vector>

#include "game/armies.h"
#include "game/game_data.h"

namespace mc
{
    // What a player (or, later, a bot) asks of the world (docs/SIMULATION.md, "Очередь команд"): plain data; an army
    // is named by its slot and that slot's generation (ArmyRef), so an order to an army that died goes to no one, even
    // after another army took its slot. A list (the provinces of a peace) is a view while the command is posted or
    // run: the queue keeps its own copy.
    enum class CommandKind : std::uint8_t
    {
        none = 0,
        move_army = 1,     // the army marches to `province` (game/movement.h)
        declare_war = 2,   // the giver declares war on `target` (game/war.h)
        peace = 3,         // the giver offers `target` a peace taking `provinces`, none: a white peace (game/peace.h)
        budget = 4,        // the giver sets its budget sliders to `shares` (game/economy/budget.h)
        maintenance = 5,   // the giver sets its land maintenance slider to `level` (game/economy/upkeep.h)
    };

    // The fields of a command that are plain values, X(type, name, default), in Command's order: from the list
    // expand Command's fields and the queue's columns, how it posts, runs and drops them; a new field is one line.
    // The list (the peace's provinces) is not a value and is written out by hand. A line's own comment is /* */.
#define MC_COMMAND_FIELDS(X)                                                                                    \
    X(CommandKind, kind, CommandKind::none)                                                                     \
    /* who gives it: it must own the army */                                                                    \
    X(std::uint16_t, country, 0)                                                                                \
    X(ArmyRef, army, {})                                                                                        \
    X(std::uint16_t, province, 0)                                                                               \
    /* a country: the one war is declared on, the one peace is offered to */                                    \
    X(std::uint16_t, target, 0)                                                                                 \
    /* the budget's, in Account's order */                                                                      \
    X(BudgetShares, shares, {})                                                                                 \
    /* the maintenance's, a percent */                                                                          \
    X(std::uint8_t, level, 0)

#define MC_DECLARE_COMMAND_FIELD(type, name, init) type name = init;
    struct Command
    {
        MC_COMMAND_FIELDS(MC_DECLARE_COMMAND_FIELD)
        std::span<const std::uint16_t> provinces;   // the peace's provinces
    };
#undef MC_DECLARE_COMMAND_FIELD

    // The one queue of commands of the game and mapcraft_sim (and later of the network): each command gets its place
    // on the time line, (day, number), the number counting the posts in their order (in the network the host will give
    // it). World runs the due ones between days, in that order (World::run_commands). Columns, one per field of
    // Command (MC_COMMAND_FIELDS), in post order; the lists one column after another (each command's first and count
    // into it). Not state: a command is executed or still due; the single game runs its commands the moment they are
    // posted, mapcraft_sim posts each day's on its day, so between days the queue is empty.
    class CommandQueue
    {
    public:
        // Queues `command` for `day` (today or later), its list copied.
        void post(std::uint32_t day, const Command& command);
        // Calls run(command) for every command due by `today` (day <= today) in (day, number) order, then drops them.
        // `run` must not post.
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
                Command command;
#define MC_READ_COMMAND_FIELD(type, name, init) command.name = name##_[i];
                MC_COMMAND_FIELDS(MC_READ_COMMAND_FIELD)
#undef MC_READ_COMMAND_FIELD
                command.provinces = std::span(list_).subspan(list_first_[i], list_count_[i]);
                run(command);
            }
            drop(today);
        }
        [[nodiscard]] bool empty() const { return day_.empty(); }

    private:
        void sort_due();                    // due_ by (day, number)
        void drop(std::uint32_t today);     // keeps the commands after today, in order

        Column<std::uint32_t> day_;
        Column<std::uint32_t> number_;
#define MC_DECLARE_COMMAND_COLUMN(type, name, init) Column<type> name##_;
        MC_COMMAND_FIELDS(MC_DECLARE_COMMAND_COLUMN)
#undef MC_DECLARE_COMMAND_COLUMN
        Column<std::uint32_t> list_first_;
        Column<std::uint32_t> list_count_;
        Column<std::uint16_t> list_;   // the lists of the commands, in post order
        std::uint32_t next_number_ = 0;
        Column<std::uint32_t> due_;    // working: the due commands' rows (reserved by post)
    };
}
