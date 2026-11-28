#pragma once

namespace mc
{
    // The showcase's own state (UI state, not the world's): what its widgets change, whether its panel is open, the GUI
    // scale it tries (0: the game's own).
    struct ShowcaseState
    {
        bool checked = true;
        bool unchecked = false;
        int slider = 50;
        int tab = 0;
        int clicks = 0;
        bool panel_open = false;
        int gui_scale = 0;
    };

    // The widget showcase (docs/UI.md, "Набор виджетов"): every widget of ui/mc_widgets.h side by side, for the user to
    // accept the look; a stand-in until the windows of the UI's frame (M8). The contents, `on_panel`: drawn on the grey
    // panel (dark text) rather than in a dark window (white text with a shadow).
    void widget_showcase(ShowcaseState* state, bool on_panel);
    // The same on a panel of its own, while state->panel_open.
    void showcase_panel(ShowcaseState* state);
}
