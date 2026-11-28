#pragma once

#include <cstdint>
#include <span>

#include "core/fixed.h"
#include "game/game_data.h"

namespace mc
{
    // The peace (docs/GAMEDATA.md, "Мир"): a country at war offers its enemy a treaty, provinces of the enemy for
    // itself or, with none, a white peace; the enemy accepts it or not by the rule below, at once, as the command's
    // effect (World): a rule of the world, the same on every machine, not a bot's choice. Until the player can answer
    // (the UI, the network) every country answers by it, the player's too. A peace ends the war and leaves a truce
    // (game/war.h). Money, vassals and the rest come with the treasury (M8) and the diplomacy (M11); EU4's desire for
    // peace with the AI (M12).

    // Why `from` cannot offer `to` a peace taking `provinces` (empty: a white peace): not at war, a province twice or
    // not `to`'s, every province `to` owns (no annexation until M11); nullptr if it can.
    [[nodiscard]] const char* why_no_peace(const WorldState& state, const AtWar& at_war, std::uint16_t from,
                                           std::uint16_t to, std::span<const std::uint16_t> provinces);

    // The treaty's price: the sum of its provinces' war values (game/war_score.h), as they are now.
    [[nodiscard]] Fixed peace_cost(const WorldState& state, const Content& content, std::uint16_t to,
                                   std::span<const std::uint16_t> provinces);

    // Whether `to` accepts (the user's rule, 28.11.2026): a demand of provinces if its price is at most `from`'s war
    // score against it; a white peace if it is losing (its war score below 0). `score`: from's war score
    // (war_score_of), `cost` the price.
    struct PeaceAnswer
    {
        bool accepted = false;
        int score = 0;
        Fixed cost = 0;
    };
    [[nodiscard]] PeaceAnswer peace_answer(const WorldState& state, const Content& content, std::uint16_t from,
                                           std::uint16_t to, std::span<const std::uint16_t> provinces);

    // The peace's effect (World, once accepted), in this order: the war ends; the battles of countries no longer at
    // war end with no winner (game/combat.h); the provinces each of the two controls of the other's go back to their
    // owners; the treaty's provinces are `from`'s, owned and controlled (cores stay); if `to`'s capital was among
    // them, its capital moves to its province of the most base_tax (equal ones: the lowest id); garrisons above their
    // fort's maximum are cut to it; the sieges that may no longer be (game/occupation.h, may_besiege) are lifted; a
    // truce until `day` + truce_years. The armies stay where they are. A province whose controller changes loses its
    // siege's progress.
    void make_peace(WorldState& state, const Content& content, AtWar& at_war, std::uint16_t from, std::uint16_t to,
                    std::span<const std::uint16_t> provinces, std::uint32_t day);
}
