#pragma once

#include <cstdint>
#include <string_view>

#include <imgui.h>

#include "ui/gui_sprites.h"

// The Minecraft widgets over Dear ImGui (docs/UI.md, "Набор виджетов"): one function a widget, drawn from the jar's GUI
// sprites (ui/gui_sprites.h) with point sampling at a whole GUI scale (a GUI pixel = scale() screen pixels), ImGui
// giving the IDs, the input, the layout and the clipping. Windows are made of these alone. A press works as in
// Minecraft and Windows: a button or a tab shows its highlighted sprite while the mouse is held down on it and over
// it, and acts when the mouse is let go over it; let go elsewhere, nothing happens. A checkbox has no highlight (only
// its tick changes), a slider lights up its handle alone while held (the user's decisions). Sizes are whole GUI
// pixels.
namespace mc::mcui
{
    // Text colours: Minecraft's white and grey on buttons, its dark grey on the grey panels.
    inline constexpr ImU32 kWhite = IM_COL32(255, 255, 255, 255);
    inline constexpr ImU32 kDisabledText = IM_COL32(160, 160, 160, 255);
    inline constexpr ImU32 kPanelText = IM_COL32(64, 64, 64, 255);

    // Each frame before the first widget (GameUi::build): the sprites and the GUI scale (whole, >= 1).
    void begin_frame(GuiSprites* sprites, int scale);
    [[nodiscard]] int scale();

    // A sprite into `rect` (screen px, whole), by its scaling (stretched, tiled, nine-sliced); nothing if the jar lacks
    // it.
    void draw_sprite(ImDrawList* list, const SpriteImage& sprite, const ImVec2& min, const ImVec2& max);
    void draw_sprite(ImDrawList* list, Sprite sprite, const ImVec2& min, const ImVec2& max);
    // Text at `at` in the current font; with a shadow as in Minecraft (one GUI pixel down and right, the colour × 1/4).
    void draw_text(ImDrawList* list, const ImVec2& at, ImU32 color, std::string_view text, bool shadow = true);

    // A window as Minecraft's containers: the grey panel with its stepped black outline, the title in dark grey (none
    // if the title shows nothing: "##top bar"), a cross to close it if `open` is given. Always pair with end_panel;
    // false: collapsed or clipped, draw nothing.
    bool begin_panel(const char* title, bool* open = nullptr, ImGuiWindowFlags flags = 0);
    // The flags of a panel the game places itself and the player does not move (the top bar, the date, the windows of
    // the zones, ui/windows.h).
    inline constexpr ImGuiWindowFlags kFixedPanel = ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoResize |
                                                    ImGuiWindowFlags_NoSavedSettings |
                                                    ImGuiWindowFlags_NoFocusOnAppearing;
    void end_panel();

    // A line of text: dark grey without a shadow on a panel by default.
    void text(std::string_view text, ImU32 color = kPanelText, bool shadow = false);
    // A button `width` GUI px wide (0: the width left), 20 high; true when clicked.
    bool button(const char* label, int width = 0, bool enabled = true);
    // A checkbox, 20 GUI px, its label to the right in the window's text colour (with a shadow when the colour is
    // light: on the dark windows; without on the panels); true when clicked (then `value` is flipped).
    bool checkbox(const char* label, bool* value);
    // A slider with "label: value" on it, `width` GUI px (0: the width left), 20 high; dragged or clicked anywhere on
    // it, its handle highlighted while held; true while the value changes.
    bool slider_int(const char* label, int* value, int min, int max, const char* format = "%d", int width = 0);
    // A tab, `width` GUI px (0: fits the label), 24 high; true when clicked. Tabs follow each other with SameLine. The
    // selected tab's label is in the window's text colour (its sprite is see-through), the others' light grey.
    bool tab(const char* label, bool selected, int width = 0);
    // A list that scrolls: an inset `height` GUI px high and the width left, Minecraft's scroller on its right
    // (dragged, or the wheel). Always pair with end_list.
    bool begin_list(const char* id, int height);
    void end_list();
    // A tooltip as Minecraft's (the dark purple box) by the cursor; text in it: text(..., kWhite, true). Call
    // end_tooltip only if it returned true.
    bool begin_tooltip();
    void end_tooltip();
    // An item's icon (textures/item/<name>), `size` GUI px square; an empty square without the jar.
    void item_icon(std::string_view item, int size = 16);
    // An image `size` screen px big that acts as a button (no highlight); true when clicked.
    bool image_button(const char* id, const SpriteImage* image, const ImVec2& size);
    // A slot (18 GUI px) with an item's icon (textures/item/<name>, empty: none) and a count (0: none).
    void slot(std::string_view item, int count = 0);
    // A bar (boss bar sprites) `width` GUI px (0: the width left), 5 high, filled to `fraction` 0..1.
    enum class BarColor : std::uint8_t
    {
        red,
        green,
        yellow,
        blue,
        white,
    };
    void bar(float fraction, BarColor color, int width = 0);
}
