#pragma once

#include <cstddef>
#include <cstdint>
#include <span>

#include "core/money.h"
#include "game/game_data.h"

// The ledger: the one way balances change (docs/ECONOMY.md, "Деньги в i64"). Money is never made or lost except by
// mint (a mine); everything else is a transfer of an exact Money between two accounts, so the world's invariant
// holds to the grain: the sum of all cash = the start + all minted.

namespace mc::ledger
{
    // The start: every pop's cash = its size x its type's start cash (pop_types.csv), rounded to grains once; the
    // counters from zero. GameData::load calls it, and the editor after each change of the pops.
    void start(const Content& content, WorldState* state);

    // `amount` >= 0 made out of nothing into `account` (only a mine does this).
    void mint(Balances* money, std::size_t account, Money amount, Flow flow);
    // Moves `amount` >= 0 from one account to another; false (and nothing moves) if `from` has less.
    bool transfer(Balances* money, std::size_t from, std::size_t to, Money amount, Flow flow);

    struct Payment
    {
        std::size_t account;
        Money amount;        // >= 0, at most what the account holds
    };
    struct Share
    {
        std::size_t account;
        double weight;       // >= 0
    };
    // A batch: every payer pays exactly its amount, and the total is shared among the payees by weight, each the
    // floor of its share; the grains the division leaves go to the payee of the largest weight (the first of equals),
    // so not one is lost. Payees of no weight at all: nobody pays.
    void settle(Balances* money, std::span<const Payment> payers, std::span<const Share> payees, Flow flow);

    // The sum of every balance.
    [[nodiscard]] Money total(const Balances& money);
    // The world's invariant: total = start + minted.
    [[nodiscard]] bool balanced(const Balances& money);

    // The cash of all pops of a province (UI, editor).
    [[nodiscard]] Money province_cash(const WorldState& state, std::uint32_t province);
}
