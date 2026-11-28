#include "minecraft/banners.h"

#include <algorithm>
#include <format>

namespace mc::minecraft
{
    namespace
    {
        // The front face of a banner texture (entity/banner/*.png, 64 x 64): the flag box's front, from (1, 1).
        bool face(const Assets& assets, const std::string& name, Image* out, std::string* error)
        {
            Image image;
            if (!assets.image("entity/banner/" + name, &image, error)) {
                return false;
            }
            if (image.width < kBannerWidth + 1 || image.height < kBannerHeight + 1) {
                if (error) {
                    *error = std::format("entity/banner/{} is {}x{}, too small for a banner", name, image.width,
                                         image.height);
                }
                return false;
            }
            out->width = kBannerWidth;
            out->height = kBannerHeight;
            out->rgba.resize(std::size_t{ kBannerWidth } * kBannerHeight * 4);
            for (std::uint32_t y = 0; y < kBannerHeight; ++y) {
                const std::uint8_t* row = image.rgba.data() + ((std::size_t{ y } + 1) * image.width + 1) * 4;
                std::copy(row, row + kBannerWidth * 4, out->rgba.data() + std::size_t{ y } * kBannerWidth * 4);
            }
            return true;
        }

        // The mask's grey times the colour, over `into` by its alpha.
        void paint(const Image& mask, std::uint32_t color, Image* into)
        {
            const std::uint32_t tint[3] = { (color >> 16) & 255, (color >> 8) & 255, color & 255 };
            for (std::size_t i = 0; i < into->rgba.size(); i += 4) {
                const std::uint32_t a = mask.rgba[i + 3];
                for (int c = 0; c < 3; ++c) {
                    const std::uint32_t top = mask.rgba[i + c] * tint[c] / 255;
                    into->rgba[i + c] = static_cast<std::uint8_t>((top * a + into->rgba[i + c] * (255 - a)) / 255);
                }
                into->rgba[i + 3] = static_cast<std::uint8_t>(a + into->rgba[i + 3] * (255 - a) / 255);
            }
        }
    }

    bool compose_banner(const Assets& assets, std::uint32_t base, std::span<const BannerLayer> layers, Image* out,
                        std::string* error)
    {
        Image mask;
        if (!face(assets, "base", &mask, error)) {
            return false;
        }
        out->width = kBannerWidth;
        out->height = kBannerHeight;
        out->rgba.assign(mask.rgba.size(), 0);
        paint(mask, base, out);
        for (const BannerLayer& layer : layers) {
            if (!face(assets, layer.pattern, &mask, error)) {
                return false;
            }
            paint(mask, layer.color, out);
        }
        return true;
    }
}
