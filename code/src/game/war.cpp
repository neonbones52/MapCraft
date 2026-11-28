#include "game/war.h"

#include <algorithm>

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
    }

    std::uint16_t war_declarer(const Wars& wars, std::uint16_t a, std::uint16_t b)
    {
        std::uint16_t declarer = 0;
        for_each_alive(wars.alive, [&](std::uint32_t w) {
            if ((wars.attacker[w] == a && wars.defender[w] == b) || (wars.attacker[w] == b && wars.defender[w] == a)) {
                declarer = wars.attacker[w];
            }
        });
        return declarer;
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

    const char* why_no_war(const WorldState& state, const AtWar& at_war, std::uint16_t attacker,
                           std::uint16_t defender)
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
        if (std::ranges::all_of(state.wars.alive, [](std::uint64_t w) { return w == ~std::uint64_t{ 0 }; })) {
            return "no war slot left";
        }
        return nullptr;
    }

    bool declare_war(WorldState& state, AtWar& at_war, std::uint16_t attacker, std::uint16_t defender,
                     std::uint32_t day)
    {
        if (why_no_war(state, at_war, attacker, defender) != nullptr) {
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
}
