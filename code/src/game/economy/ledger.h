#pragma once

#include <cstddef>
#include <cstdint>
#include <span>

#include "core/money.h"
#include "game/game_data.h"

// The ledger: the one way balances change (docs/ECONOMY.md, "Деньги в i64"). Money is not closed (EU3-lite): mint
// makes income out of nothing; everything else moves an exact Money between two accounts. The accounts are the
// countries' (game/game_data.h, account()): the census mints into the treasury, the monthly income is minted into
// the five accounts by the budget sliders (mint_shares; game/economy/budget.h).

namespace mc::ledger
{
    // `amount` >= 0 made out of nothing into `account` (taxes, production, trade, the Treasury share).
    void mint(Balances* money, std::size_t account, Money amount);
    // `amount` >= 0 made out of nothing and shared among `accounts` by whole `weights` (as many, not all 0): each
    // gets the floor of its share, worked out in whole numbers; the grains the division leaves go to the account of
    // the largest weight (the first of equals), so the shares add up to `amount` exactly. All weights 0: nothing.
    void mint_shares(Balances* money, std::span<const std::size_t> accounts, Money amount,
                     std::span<const std::uint8_t> weights);
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
