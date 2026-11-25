#pragma once

#include <cstddef>
#include <cstdint>
#include <span>

#include "core/money.h"
#include "game/game_data.h"

// The ledger: the one way balances change (docs/ECONOMY.md, "Деньги в i64"). Money is not closed (EU3-lite): mint
// makes income out of nothing; everything else moves an exact Money between two accounts. No rule uses it yet: the
// accounts come with the treasury (M8).

namespace mc::ledger
{
    // `amount` >= 0 made out of nothing into `account` (taxes, production, trade, minting).
    void mint(Balances* money, std::size_t account, Money amount);
    // Moves `amount` >= 0 from one account to another; false (and nothing moves) if `from` has less.
    bool transfer(Balances* money, std::size_t from, std::size_t to, Money amount);

    // A batch, in columns: payer i pays exactly amounts[i] (>= 0, at most what it holds), and the total is shared among
    // the payees by weight (>= 0), each the floor of its share; the grains the division leaves go to the payee of the
    // largest weight (the first of equals), so not one is lost. Payees of no weight at all: nobody pays.
    void settle(Balances* money, std::span<const std::uint32_t> payers, std::span<const Money> amounts,
                std::span<const std::uint32_t> payees, std::span<const double> weights);

    // The sum of every balance.
    [[nodiscard]] Money total(const Balances& money);
}
