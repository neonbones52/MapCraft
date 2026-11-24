#pragma once

#include "game/game_data.h"

// The economy's day: its phases, in the one order written in economy.cpp (docs/ECONOMY.md, "Код").

namespace mc
{
    void economy_day(const Content& content, WorldState* state);
}
