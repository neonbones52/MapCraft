#pragma once

#include <cstdint>

// The simulation's randomness (CLAUDE.md, rule 3; docs/SIMULATION.md, "Seed и случайность"): a counter-based
// generator with no state. A draw is a pure function of (the world's seed, the day, the entity, the purpose), so one
// more draw somewhere never shifts any other, and every machine draws the same. Integer mixing only (SplitMix64's
// finaliser over the four words in turn).

namespace mc
{
    [[nodiscard]] constexpr std::uint64_t random_mix(std::uint64_t z)
    {
        z = (z ^ (z >> 30)) * 0xbf58476d1ce4e5b9ull;
        z = (z ^ (z >> 27)) * 0x94d049bb133111ebull;
        return z ^ (z >> 31);
    }

    // 64 random bits for (seed, day, entity, purpose). `entity` names what draws (a battle: its slot and generation),
    // `purpose` what for (each rule its own constant, game/combat.h).
    [[nodiscard]] constexpr std::uint64_t random(std::uint32_t seed, std::uint32_t day, std::uint32_t entity,
                                                 std::uint32_t purpose)
    {
        std::uint64_t h = random_mix(0x9e3779b97f4a7c15ull ^ seed);
        h = random_mix(h ^ (std::uint64_t{ day } << 32 | entity));
        return random_mix(h ^ (0x632be59bd9b4e019ull + purpose));
    }

    // A whole number in [0, n), n > 0 (the bias of the modulo is below 2^-60 for the small n of dice).
    [[nodiscard]] constexpr std::uint32_t random_below(std::uint32_t seed, std::uint32_t day, std::uint32_t entity,
                                                       std::uint32_t purpose, std::uint32_t n)
    {
        return static_cast<std::uint32_t>(random(seed, day, entity, purpose) % n);
    }
}
