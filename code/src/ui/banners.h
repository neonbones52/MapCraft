#pragma once

#include <cstdint>
#include <map>

#include "ui/gui_sprites.h"

namespace mc
{
    struct CountryDefinitions;

    // The banners the UI shows (docs/UI.md, "Раскладка экрана"; minecraft/banners.h), each made once and kept as a
    // texture. A country's: for now its map colour alone (its own colours and patterns come with the data, later); the
    // observer's (no country): split from the top left to the bottom right, the lower left half black, the upper right
    // dark grey. nullptr without the jar.
    class BannerCache
    {
    public:
        void init(GuiSprites* sprites) { sprites_ = sprites; }
        const SpriteImage* country(const CountryDefinitions& countries, std::uint16_t country);
        const SpriteImage* observer();

    private:
        GuiSprites* sprites_ = nullptr;
        std::map<std::uint32_t, SpriteImage> made_;   // by country, observer = kObserver
    };
}
