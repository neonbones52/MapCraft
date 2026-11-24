#include "game/economy/ledger.h"

#include <cmath>

namespace mc::ledger
{
    void start(const Content& content, WorldState* state)
    {
        const Pops& pops = state->pops;
        Balances& money = state->money;
        money.cash.assign(pops.size.size(), 0);
        money.start = 0;
        money.minted = 0;
        money.flows = {};
        for (std::size_t t = 1; t < pops.types && t < content.pop_types.size(); ++t) {
            const double per_person = content.pop_types.start_cash[t];
            for (std::size_t p = 0; p < pops.stride; ++p) {
                const std::size_t account = t * pops.stride + p;
                money.cash[account] = to_grains(static_cast<double>(pops.size[account]) * per_person);
                money.start += money.cash[account];
            }
        }
    }

    void mint(Balances* money, std::size_t account, Money amount, Flow flow)
    {
        money->cash[account] += amount;
        money->minted += amount;
        money->flows[static_cast<std::size_t>(flow)] += amount;
    }

    bool transfer(Balances* money, std::size_t from, std::size_t to, Money amount, Flow flow)
    {
        if (money->cash[from] < amount) {
            return false;
        }
        money->cash[from] -= amount;
        money->cash[to] += amount;
        money->flows[static_cast<std::size_t>(flow)] += amount;
        return true;
    }

    void settle(Balances* money, std::span<const std::uint32_t> payers, std::span<const Money> amounts,
                std::span<const std::uint32_t> payees, std::span<const double> weights, Flow flow)
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
        money->flows[static_cast<std::size_t>(flow)] += total;
    }

    Money total(const Balances& money)
    {
        Money sum = 0;
        for (const Money m : money.cash) {
            sum += m;
        }
        return sum;
    }

    bool balanced(const Balances& money)
    {
        return total(money) == money.start + money.minted;
    }

    Money province_cash(const WorldState& state, std::uint32_t province)
    {
        Money sum = 0;
        for (std::size_t t = 1; t < state.pops.types; ++t) {
            sum += state.money.cash[t * state.pops.stride + province];
        }
        return sum;
    }
}
