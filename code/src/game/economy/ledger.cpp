#include "game/economy/ledger.h"

#include <cmath>

namespace mc::ledger
{
    void mint(Balances* money, std::size_t account, Money amount)
    {
        money->cash[account] += amount;
    }

    void mint_shares(Balances* money, std::span<const std::size_t> accounts, Money amount,
                     std::span<const std::uint8_t> weights)
    {
        Money weight = 0;
        std::size_t largest = 0;
        for (std::size_t i = 0; i < weights.size(); ++i) {
            weight += weights[i];
            if (weights[i] > weights[largest]) {
                largest = i;
            }
        }
        if (weight == 0) {
            return;
        }
        // amount = whole × weight + rest: a share is whole × w + floor(rest × w / weight), with no product that
        // could overflow (rest < weight).
        const Money whole = amount / weight;
        const Money rest = amount % weight;
        Money given = 0;
        for (std::size_t i = 0; i < accounts.size(); ++i) {
            const Money part = whole * weights[i] + rest * weights[i] / weight;
            money->cash[accounts[i]] += part;
            given += part;
        }
        money->cash[accounts[largest]] += amount - given;
    }

    void spend(Balances* money, std::size_t account, Money amount)
    {
        money->cash[account] -= amount;
    }

    bool transfer(Balances* money, std::size_t from, std::size_t to, Money amount)
    {
        if (money->cash[from] < amount) {
            return false;
        }
        money->cash[from] -= amount;
        money->cash[to] += amount;
        return true;
    }

    void settle(Balances* money, std::span<const std::uint32_t> payers, std::span<const Money> amounts,
                std::span<const std::uint32_t> payees, std::span<const double> weights)
    {
        double weight = 0.0;
        std::size_t largest = 0;
        for (std::size_t i = 0; i < weights.size(); ++i) {
            weight += weights[i];
            if (weights[i] > weights[largest]) {
                largest = i;
            }
        }
        if (payees.empty() || weight <= 0.0) {
            return;
        }
        Money total = 0;
        for (std::size_t i = 0; i < payers.size(); ++i) {
            money->cash[payers[i]] -= amounts[i];
            total += amounts[i];
        }
        Money given = 0;
        for (std::size_t i = 0; i < payees.size(); ++i) {
            const auto part = static_cast<Money>(std::floor(static_cast<double>(total) * (weights[i] / weight)));
            money->cash[payees[i]] += part;
            given += part;
        }
        money->cash[payees[largest]] += total - given;
    }

    Money total(const Balances& money)
    {
        Money sum = 0;
        for (const Money m : money.cash) {
            sum += m;
        }
        return sum;
    }
}
