#pragma once

#include "ui/screens/screen.h"
#include "view/army_marker.h"

namespace mc
{
    // The plate under each army's figure, as in EU4 (both phases): the owner's flag, the men, a bar of the army's
    // average morale against its full; the plate's colour says whose army it is to the player (own; enemy: before
    // diplomacy every foreign army once the player plays; neutral while choosing a country); a selected army's plate is framed in white. Drawn on the background draw list, so
    // under every window; it always faces the screen. They fade with the figures (`armies.fade`). Over them, the box
    // being dragged to select armies. Returns the army whose plate is under the cursor (the top one), -1 for none.
    int army_plates(const ScreenContext& ui, const ArmyMarkers& armies, float cursor_x, float cursor_y);
}
