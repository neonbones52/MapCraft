#include "game/economy/budget.h"

#include <array>

#include "core/money.h"
#include "game/calendar.h"
#include "game/economy/ledger.h"

namespace mc
{
    void Budget::census(WorldState& state, const Content& content)
    {
        world_income(state, content, &income_);
        for (std::size_t c = 1; c < state.countries.count; ++c) {
            ledger::mint(&state.money, account(c, Account::treasury), to_grains(income_.sum[c].census));
        }
    }

    void Budget::month(WorldState& state, const Content& content)
    {
        world_income(state, content, &income_);
        CountryStates& countries = state.countries;
        const double per_month = fx_to_double(content.economy.treasury_inflation) / static_cast<double>(kMonthsPerYear);
        for (std::size_t c = 1; c < countries.count; ++c) {
            const Money income = to_grains(income_.sum[c].monthly());
            if (income <= 0) {
                continue;
            }
            std::array<std::size_t, kAccountsPerCountry> accounts{};
            for (std::size_t a = 0; a < kAccountsPerCountry; ++a) {
                accounts[a] = account(c, static_cast<Account>(a));
            }
            const BudgetShares shares = budget_shares(countries, static_cast<std::uint16_t>(c));
            ledger::mint_shares(&state.money, accounts, income, shares);
            const std::uint8_t treasury = shares[static_cast<std::size_t>(Account::treasury)];
            if (treasury > 0) {
                countries.inflation[c] += static_cast<float>(static_cast<double>(treasury) / 100.0 * per_month);
            }
        }
    }

    BudgetShares budget_shares(const CountryStates& countries, std::uint16_t country)
    {
        BudgetShares shares{};
        for (std::size_t a = 0; a < kAccountsPerCountry; ++a) {
            shares[a] = (countries.*kBudgetShares[a])[country];
        }
        return shares;
    }

    const char* why_no_budget(const WorldState& state, std::uint16_t country, const BudgetShares& shares)
    {
        if (country == 0 || country >= state.countries.count) {
            return "no such country";
        }
        int sum = 0;
        for (const std::uint8_t s : shares) {
            sum += s;
        }
        if (sum != 100) {
            return "the shares do not add up to 100";
        }
        return nullptr;
    }

    void set_budget(WorldState& state, std::uint16_t country, const BudgetShares& shares)
    {
        for (std::size_t a = 0; a < kAccountsPerCountry; ++a) {
            (state.countries.*kBudgetShares[a])[country] = shares[a];
        }
    }
}
