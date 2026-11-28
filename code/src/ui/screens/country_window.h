#pragma once

#include <cstdint>

#include "ui/screens/screen.h"

namespace mc
{
    class Windows;

    // The country window's tabs (EU3's and EU4's ledger of a country, docs/UI.md, "Окна").
    enum class CountryTab : std::uint8_t
    {
        government,
        economy,
        military,
        diplomacy,
        count,
    };

    // Its own state (UI state, not the world's): the tab shown.
    struct CountryWindowState
    {
        CountryTab tab = CountryTab::government;
    };

    // The player's country (GameWindow::country, the left zone): opened by the banner, the screen's height under it.
    // A frame for now (the user's decision 28.11.2026: what each tab holds comes later): what the game already knows,
    // read only; the actions stay in the Dev window until their places are decided. Government: the capital, the
    // provinces and cores, "Play another country"; Economy: the treasury, the income, the census, the inflation, the
    // investments and the budget sliders; Military: the manpower, the force limit, the regiments, the upkeep and the
    // maintenance, the regiments being raised; Diplomacy: the wars and their war scores.
    void country_window(const ScreenContext& ui, Windows* windows, CountryWindowState* state, UiCommands* commands);
}
