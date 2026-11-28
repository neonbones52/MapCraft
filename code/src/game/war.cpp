#include "game/war.h"

#include <algorithm>
#include <utility>

#include "game/armies.h"

namespace mc
{
    namespace
    {
        void set_pair(AtWar& at_war, std::size_t a, std::size_t b)
        {
            at_war.bits[a * at_war.words + b / 64] |= std::uint64_t{ 1 } << (b % 64);
            at_war.bits[b * at_war.words + a / 64] |= std::uint64_t{ 1 } << (a % 64);
        }

        // The truce's slot of `a` and `b` (whether it ran out or not), -1 if none.
        int truce_slot(const Truces& truces, std::uint16_t a, std::uint16_t b)
        {
            const std::uint16_t first = std::min(a, b);
            const std::uint16_t second = std::max(a, b);
            int found = -1;
            for_each_alive(truces.alive, [&](std::uint32_t t) {
                if (truces.first[t] == first && truces.second[t] == second) {
                    found = static_cast<int>(t);
                }
            });
            return found;
        }

        void clear_truce(Truces& truces, std::uint32_t slot)
        {
#define MC_CLEAR_SLOT(table, type, name, start) truces.name[slot] = type{};
            MC_TRUCE_COLUMNS(MC_CLEAR_SLOT)
#undef MC_CLEAR_SLOT
            free_slot(truces.alive, slot);
        }
    }

    int war_between(const Wars& wars, std::uint16_t a, std::uint16_t b)
    {
        int found = -1;
        for_each_alive(wars.alive, [&](std::uint32_t w) {
            if ((wars.attacker[w] == a && wars.defender[w] == b) || (wars.attacker[w] == b && wars.defender[w] == a)) {
                found = static_cast<int>(w);
            }
        });
        return found;
    }

    std::uint16_t war_declarer(const Wars& wars, std::uint16_t a, std::uint16_t b)
    {
        const int w = war_between(wars, a, b);
        return w < 0 ? std::uint16_t{ 0 } : wars.attacker[static_cast<std::size_t>(w)];
    }

    void rebuild_at_war(const Wars& wars, std::size_t country_count, AtWar* out)
    {
        out->count = country_count;
        out->words = static_cast<std::uint32_t>(std::max<std::size_t>((country_count + 63) / 64, 1));
        out->bits.assign(std::max<std::size_t>(country_count, 1) * out->words, 0);
        for_each_alive(wars.alive, [&](std::uint32_t w) {
            if (wars.attacker[w] < country_count && wars.defender[w] < country_count) {
                set_pair(*out, wars.attacker[w], wars.defender[w]);
            }
        });
    }

    std::uint32_t truce_until(const Truces& truces, std::uint16_t a, std::uint16_t b, std::uint32_t day)
    {
        const int t = truce_slot(truces, a, b);
        if (t < 0) {
            return 0;
        }
        const std::uint32_t until = truces.until[static_cast<std::size_t>(t)];
        return until > day ? until : 0;
    }

    const char* why_no_war(const WorldState& state, const AtWar& at_war, std::uint16_t attacker,
                           std::uint16_t defender, std::uint32_t day)
    {
        const std::size_t countries = state.countries.count;
        if (attacker == 0 || defender == 0 || attacker >= countries || defender >= countries) {
            return "no such country";
        }
        if (attacker == defender) {
            return "a war on itself";
        }
        if (at_war.has(attacker, defender)) {
            return "already at war";
        }
        if (truce_until(state.truces, attacker, defender, day) != 0) {
            return "a truce";
        }
        if (std::ranges::all_of(state.wars.alive, [](std::uint64_t w) { return w == ~std::uint64_t{ 0 }; })) {
            return "no war slot left";
        }
        return nullptr;
    }

    bool declare_war(WorldState& state, AtWar& at_war, std::uint16_t attacker, std::uint16_t defender,
                     std::uint32_t day)
    {
        if (why_no_war(state, at_war, attacker, defender, day) != nullptr) {
            return false;
        }
        Wars& wars = state.wars;
        std::uint16_t slot = 0;
        if (!take_slot(wars.alive, &slot)) {
            return false;
        }
        wars.attacker[slot] = attacker;
        wars.defender[slot] = defender;
        wars.start[slot] = day;
        set_pair(at_war, attacker, defender);
        return true;
    }

    void end_war(WorldState& state, AtWar& at_war, std::uint16_t slot)
    {
        Wars& wars = state.wars;
        const std::uint16_t generation = wars.generation[slot];
#define MC_CLEAR_SLOT(table, type, name, start) wars.name[slot] = type{};
        MC_WAR_COLUMNS(MC_CLEAR_SLOT)
#undef MC_CLEAR_SLOT
        wars.generation[slot] = static_cast<std::uint16_t>(generation + 1);
        free_slot(wars.alive, slot);
        rebuild_at_war(wars, at_war.count, &at_war);
    }

    void set_truce(Truces& truces, std::uint16_t a, std::uint16_t b, std::uint32_t until)
    {
        int slot = truce_slot(truces, a, b);
        if (slot < 0) {
            std::uint16_t taken = 0;
            if (take_slot(truces.alive, &taken)) {
                slot = taken;
            } else {
                // No slot left: the truce that runs out first gives way.
                std::uint32_t soonest = 0;
                for_each_alive(truces.alive, [&](std::uint32_t t) {
                    if (slot < 0 || truces.until[t] < soonest) {
                        slot = static_cast<int>(t);
                        soonest = truces.until[t];
                    }
                });
            }
        }
        const auto s = static_cast<std::size_t>(slot);
        truces.first[s] = std::min(a, b);
        truces.second[s] = std::max(a, b);
        truces.until[s] = until;
    }

    void expire_truces(Truces& truces, std::uint32_t day)
    {
        for_each_alive(truces.alive, [&](std::uint32_t t) {
            if (truces.until[t] <= day) {
                clear_truce(truces, t);
            }
        });
    }
}
