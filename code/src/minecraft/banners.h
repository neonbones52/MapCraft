#pragma once

#include <cstdint>
#include <span>
#include <string>

#include "minecraft/assets.h"

// Banners (docs/UI.md, "Раскладка экрана"): the front of Minecraft's banner, entity/banner/base.png's 20 x 40 face,
// with patterns over it, each a mask of the same face (entity/banner/<pattern>.png, a grey shape on transparency)
// multiplied by its colour. Any colours, not only Minecraft's 16 dyes (the user's decision); more masks than the jar's
// can come later.
namespace mc::minecraft
{
    // The face's size, px.
    inline constexpr std::uint32_t kBannerWidth = 20;
    inline constexpr std::uint32_t kBannerHeight = 40;

    struct BannerLayer
    {
        std::string pattern;      // a mask of entity/banner/ ("diagonal_up_left")
        std::uint32_t color = 0;  // 0xRRGGBB
    };

    // The banner's face: the base in `base` (0xRRGGBB), then the layers in order. False if the jar lacks a texture.
    bool compose_banner(const Assets& assets, std::uint32_t base, std::span<const BannerLayer> layers, Image* out,
                        std::string* error);
}
