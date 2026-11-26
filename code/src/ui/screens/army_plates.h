#pragma once

#include "ui/screens/screen.h"
#include "view/army_marker.h"

namespace mc
{
    // The plate under each army's figure, as in EU4 (both phases): the owner's flag, the men, a bar of the army's
    // average morale against its full; the plate's colour says whose army it is to the player (own or neutral; enemy
    // once there are wars, M7). Drawn on the background draw list, so under every window; it always faces the screen.
    // They fade with the figures (`armies.fade`).
    void army_plates(const ScreenContext& ui, const ArmyMarkers& armies);
}
