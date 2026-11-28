#pragma once

#include <array>
#include <cstdint>
#include <filesystem>
#include <map>
#include <memory>
#include <string>
#include <string_view>

#include <d3d11.h>
#include <wrl/client.h>

#include "minecraft/assets.h"

namespace mc
{
    // The Minecraft GUI sprites the widgets are made of (docs/UI.md, "Набор виджетов"), read from the player's jar
    // when the game starts (never copied: CLAUDE.md, "Ассеты"), each its own texture with its scaling from .mcmeta;
    // the boss bars, which have none, are cut in three (2 GUI px at each end) so that they take any width. The list is
    // the code's: a new sprite is one line in kSpriteNames (gui_sprites.cpp) and one here.
    enum class Sprite : std::uint8_t
    {
        button,
        button_highlighted,
        button_disabled,
        slider,
        slider_handle,
        slider_handle_highlighted,
        checkbox,
        checkbox_selected,
        tab,
        tab_highlighted,
        tab_selected,
        tab_selected_highlighted,
        scroller,
        scroller_background,
        tooltip_background,
        tooltip_frame,
        cross_button,
        cross_button_highlighted,
        slot,
        bar_red_background,
        bar_red_progress,
        bar_green_background,
        bar_green_progress,
        bar_yellow_background,
        bar_yellow_progress,
        bar_blue_background,
        bar_blue_progress,
        bar_white_background,
        bar_white_progress,
        // Made by the code from Minecraft's own pixels (not in the jar as sprites): the panel of its containers (the
        // grey, the white light, the #555555 shade, the stepped black outline; gui/container/*.png) and the inset of
        // its slots (#373737 above and left, white below and right, #8B8B8B inside).
        panel,
        inset,
        count,
    };

    struct SpriteImage
    {
        Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> texture;   // null: the jar lacks it (drawn as nothing)
        std::uint32_t width = 0;    // the image's pixels
        std::uint32_t height = 0;
        minecraft::GuiScaling scaling;
    };

    class GuiSprites
    {
    public:
        GuiSprites();
        ~GuiSprites();
        GuiSprites(const GuiSprites&) = delete;
        GuiSprites& operator=(const GuiSprites&) = delete;

        // Every sprite of the list from `jar`; a missing one is a warning (the widget then draws without it). False
        // (and no sprites) if the jar cannot be opened. The jar stays open for the item icons.
        bool load(ID3D11Device* device, const std::filesystem::path& jar);
        [[nodiscard]] bool loaded() const { return assets_ != nullptr; }

        [[nodiscard]] const SpriteImage& operator[](Sprite sprite) const
        {
            return sprites_[static_cast<std::size_t>(sprite)];
        }
        // An item's icon, textures/item/<name>.png ("emerald"): loaded on first use, then kept; nullptr if the jar
        // has none.
        const SpriteImage* item(std::string_view name);
        // The jar while it is open (the banners, ui/banners.h); nullptr without one.
        [[nodiscard]] const minecraft::Assets* assets() const { return assets_.get(); }
        [[nodiscard]] ID3D11Device* device() const { return device_.Get(); }

    private:
        Microsoft::WRL::ComPtr<ID3D11Device> device_;
        std::unique_ptr<minecraft::Assets> assets_;
        std::array<SpriteImage, static_cast<std::size_t>(Sprite::count)> sprites_;
        std::map<std::string, SpriteImage, std::less<>> items_;
    };
}
