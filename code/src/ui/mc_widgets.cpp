#include "ui/mc_widgets.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

#include <imgui_internal.h>   // ButtonBehavior, ItemAdd, SetScrollY: widgets of our own

namespace mc::mcui
{
    namespace
    {
        GuiSprites* g_sprites = nullptr;
        int g_scale = 1;

        // The lists begun and not ended: the inset's corners and the scroller's ID.
        struct ListFrame
        {
            ImVec2 min;
            ImVec2 max;
            ImGuiID scroller = 0;
        };
        std::vector<ListFrame> g_lists;

        float px(int gui) { return static_cast<float>(gui * g_scale); }
        ImVec2 add(const ImVec2& a, const ImVec2& b) { return ImVec2(a.x + b.x, a.y + b.y); }
        ImVec2 snap(const ImVec2& p) { return ImVec2(std::floor(p.x), std::floor(p.y)); }

        // The width `gui` GUI px, or the width left in whole GUI px.
        float width_of(int gui)
        {
            if (gui > 0) {
                return px(gui);
            }
            const float left = ImGui::GetContentRegionAvail().x;
            return std::max(1.0f, std::floor(left / static_cast<float>(g_scale))) * static_cast<float>(g_scale);
        }

        ImTextureRef texture_of(const SpriteImage& sprite)
        {
            return ImTextureRef(reinterpret_cast<ImTextureID>(sprite.texture.Get()));
        }

        // The sprites are pixel art: point sampling while they are drawn, the font's linear sampling back after.
        void nearest(ImDrawList* list)
        {
            if (const ImDrawCallback set = ImGui::GetPlatformIO().DrawCallback_SetSamplerNearest) {
                list->AddCallback(set, nullptr);
            }
        }
        void linear(ImDrawList* list)
        {
            if (const ImDrawCallback set = ImGui::GetPlatformIO().DrawCallback_SetSamplerLinear) {
                list->AddCallback(set, nullptr);
            }
        }

        // The texture's part uv0..uv1, `tile` screen px big, repeated over min..max from the top left; the last row and
        // column cut short.
        void tile(ImDrawList* list, ImTextureRef texture, const ImVec2& min, const ImVec2& max, const ImVec2& uv0,
                  const ImVec2& uv1, const ImVec2& size)
        {
            if (size.x <= 0.0f || size.y <= 0.0f) {
                return;
            }
            for (float y = min.y; y < max.y; y += size.y) {
                const float h = std::min(size.y, max.y - y);
                const float v1 = uv0.y + (uv1.y - uv0.y) * h / size.y;
                for (float x = min.x; x < max.x; x += size.x) {
                    const float w = std::min(size.x, max.x - x);
                    const float u1 = uv0.x + (uv1.x - uv0.x) * w / size.x;
                    list->AddImage(texture, ImVec2(x, y), ImVec2(x + w, y + h), uv0, ImVec2(u1, v1));
                }
            }
        }

        // The visible part of a label: up to "##".
        std::string_view visible(const char* label)
        {
            const std::string_view text(label);
            return text.substr(0, text.find("##"));
        }

        // The colour of the window's text and whether it takes a shadow (light text, on the dark windows).
        ImU32 label_color(bool* shadow)
        {
            const ImVec4 c = ImGui::GetStyleColorVec4(ImGuiCol_Text);
            *shadow = 0.299f * c.x + 0.587f * c.y + 0.114f * c.z > 0.5f;
            return ImGui::GetColorU32(c);
        }

        // Disabled by the caller (ImGui::BeginDisabled) or by the argument.
        bool enabled_now(bool enabled)
        {
            return enabled && (ImGui::GetCurrentContext()->CurrentItemFlags & ImGuiItemFlags_Disabled) == 0;
        }

        // Text centred in min..max.
        void centred(ImDrawList* list, const ImVec2& min, const ImVec2& max, ImU32 color, std::string_view text)
        {
            const ImVec2 size = ImGui::CalcTextSize(text.data(), text.data() + text.size());
            const ImVec2 at = snap(ImVec2((min.x + max.x - size.x) * 0.5f, (min.y + max.y - size.y) * 0.5f));
            list->PushClipRect(min, max, true);
            draw_text(list, at, color, text, true);
            list->PopClipRect();
        }

        // Reserves a widget's room at the cursor (whole pixels) and registers it; false if it is clipped.
        bool place(ImGuiID id, const ImVec2& size, ImRect* bb)
        {
            const ImVec2 at = snap(ImGui::GetCursorScreenPos());
            *bb = ImRect(at, add(at, size));
            ImGui::ItemSize(size);
            return ImGui::ItemAdd(*bb, id);
        }
    }

    void begin_frame(GuiSprites* sprites, int scale)
    {
        g_sprites = sprites;
        g_scale = std::max(1, scale);
        g_lists.clear();
    }

    int scale()
    {
        return g_scale;
    }

    void draw_sprite(ImDrawList* list, const SpriteImage& sprite, const ImVec2& min, const ImVec2& max)
    {
        if (!sprite.texture || max.x <= min.x || max.y <= min.y) {
            return;
        }
        const minecraft::GuiScaling& sc = sprite.scaling;
        const float s = static_cast<float>(g_scale);
        const float layout_w = static_cast<float>(std::max<std::uint32_t>(sc.width, 1));
        const float layout_h = static_cast<float>(std::max<std::uint32_t>(sc.height, 1));
        const ImTextureRef texture = texture_of(sprite);
        nearest(list);
        switch (sc.kind) {
        case minecraft::GuiScaling::Kind::stretch:
            list->AddImage(texture, min, max);
            break;
        case minecraft::GuiScaling::Kind::tile:
            tile(list, texture, min, max, ImVec2(0, 0), ImVec2(1, 1), ImVec2(layout_w * s, layout_h * s));
            break;
        case minecraft::GuiScaling::Kind::nine_slice: {
            // Borders in screen px; a target smaller than two borders takes half of it for each (as Minecraft).
            const float w = max.x - min.x;
            const float h = max.y - min.y;
            float left = static_cast<float>(sc.left) * s;
            float right = static_cast<float>(sc.right) * s;
            float top = static_cast<float>(sc.top) * s;
            float bottom = static_cast<float>(sc.bottom) * s;
            if (left + right > w) {
                left = std::floor(w * 0.5f);
                right = w - left;
            }
            if (top + bottom > h) {
                top = std::floor(h * 0.5f);
                bottom = h - top;
            }
            const float xs[4] = { min.x, min.x + left, max.x - right, max.x };
            const float ys[4] = { min.y, min.y + top, max.y - bottom, max.y };
            const float us[4] = { 0.0f, left / s / layout_w, 1.0f - right / s / layout_w, 1.0f };
            const float vs[4] = { 0.0f, top / s / layout_h, 1.0f - bottom / s / layout_h, 1.0f };
            for (int j = 0; j < 3; ++j) {
                for (int i = 0; i < 3; ++i) {
                    const ImVec2 a(xs[i], ys[j]);
                    const ImVec2 b(xs[i + 1], ys[j + 1]);
                    if (b.x <= a.x || b.y <= a.y) {
                        continue;
                    }
                    const ImVec2 uv0(us[i], vs[j]);
                    const ImVec2 uv1(us[i + 1], vs[j + 1]);
                    const bool corner = i != 1 && j != 1;
                    if (!corner && sc.stretch_inner) {
                        list->AddImage(texture, a, b, uv0, uv1);
                    } else {
                        tile(list, texture, a, b, uv0, uv1,
                             ImVec2((uv1.x - uv0.x) * layout_w * s, (uv1.y - uv0.y) * layout_h * s));
                    }
                }
            }
            break;
        }
        }
        linear(list);
    }

    void draw_sprite(ImDrawList* list, Sprite sprite, const ImVec2& min, const ImVec2& max)
    {
        if (g_sprites != nullptr) {
            draw_sprite(list, (*g_sprites)[sprite], min, max);
        }
    }

    void draw_text(ImDrawList* list, const ImVec2& at, ImU32 color, std::string_view text, bool shadow)
    {
        const char* begin = text.data();
        const char* end = text.data() + text.size();
        const ImVec2 p = snap(at);
        if (shadow) {
            // Minecraft's: the colour's channels / 4, one GUI pixel down and right.
            const ImU32 dark = ((color & 0x00FCFCFCu) >> 2) | (color & 0xFF000000u);
            list->AddText(add(p, ImVec2(px(1), px(1))), dark, begin, end);
        }
        list->AddText(p, color, begin, end);
    }

    bool begin_panel(const char* title, bool* open, ImGuiWindowFlags flags)
    {
        ImGui::PushStyleColor(ImGuiCol_Text, kPanelText);
        ImGui::PushStyleColor(ImGuiCol_WindowBg, IM_COL32(0, 0, 0, 0));
        ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(px(7), px(6)));
        const bool shown = ImGui::Begin(title, nullptr, flags | ImGuiWindowFlags_NoTitleBar |
                                                            ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoScrollbar);
        ImGui::PopStyleVar(2);
        ImGui::PopStyleColor();   // the background's (read by Begin); the text colour stays until end_panel
        // The panel goes under everything in it: drawn at the end into the channel below.
        ImDrawList* list = ImGui::GetWindowDrawList();
        list->ChannelsSplit(2);
        list->ChannelsSetCurrent(1);
        if (!shown) {
            return false;
        }
        const std::string_view name = visible(title);
        if (!name.empty()) {
            const ImVec2 at = snap(ImGui::GetCursorScreenPos());
            const ImVec2 size = ImGui::CalcTextSize(name.data(), name.data() + name.size());
            draw_text(list, at, kPanelText, name, false);
            ImGui::Dummy(ImVec2(size.x, size.y + px(2)));
        }
        if (open != nullptr) {
            // The cross at the top right corner, inside the outline.
            const ImVec2 pos = ImGui::GetWindowPos();
            const float right = pos.x + ImGui::GetWindowSize().x - px(4);
            const ImRect bb(snap(ImVec2(right - px(14), pos.y + px(4))), snap(ImVec2(right, pos.y + px(18))));
            const ImGuiID id = ImGui::GetID("##close");
            if (ImGui::ItemAdd(bb, id)) {
                bool hovered = false;
                bool held = false;
                if (ImGui::ButtonBehavior(bb, id, &hovered, &held)) {
                    *open = false;
                }
                draw_sprite(list, held && hovered ? Sprite::cross_button_highlighted : Sprite::cross_button, bb.Min,
                            bb.Max);
            }
        }
        return true;
    }

    void end_panel()
    {
        ImDrawList* list = ImGui::GetWindowDrawList();
        list->ChannelsSetCurrent(0);
        const ImVec2 pos = snap(ImGui::GetWindowPos());
        const ImVec2 size = snap(ImGui::GetWindowSize());
        const ImVec2 end = add(pos, size);
        list->PushClipRect(pos, end, false);
        draw_sprite(list, Sprite::panel, pos, end);
        list->PopClipRect();
        list->ChannelsMerge();
        ImGui::End();
        ImGui::PopStyleColor();
    }

    void text(std::string_view text, ImU32 color, bool shadow)
    {
        if (ImGui::GetCurrentWindow()->SkipItems) {
            return;
        }
        const ImVec2 at = snap(ImGui::GetCursorScreenPos());
        const ImVec2 size = ImGui::CalcTextSize(text.data(), text.data() + text.size());
        ImGui::Dummy(ImVec2(size.x + (shadow ? px(1) : 0.0f), size.y));
        if (ImGui::IsItemVisible()) {
            draw_text(ImGui::GetWindowDrawList(), at, color, text, shadow);
        }
    }

    bool button(const char* label, int width, bool enabled)
    {
        ImGuiWindow* window = ImGui::GetCurrentWindow();
        if (window->SkipItems) {
            return false;
        }
        const ImGuiID id = window->GetID(label);
        ImRect bb;
        if (!place(id, ImVec2(width_of(width), px(20)), &bb)) {
            return false;
        }
        const bool on = enabled_now(enabled);
        bool hovered = false;
        bool held = false;
        const bool pressed = on && ImGui::ButtonBehavior(bb, id, &hovered, &held);
        const Sprite sprite = !on ? Sprite::button_disabled : held && hovered ? Sprite::button_highlighted : Sprite::button;
        draw_sprite(window->DrawList, sprite, bb.Min, bb.Max);
        centred(window->DrawList, bb.Min, bb.Max, on ? kWhite : kDisabledText, visible(label));
        return pressed;
    }

    bool checkbox(const char* label, bool* value)
    {
        ImGuiWindow* window = ImGui::GetCurrentWindow();
        if (window->SkipItems) {
            return false;
        }
        const ImGuiID id = window->GetID(label);
        const std::string_view name = visible(label);
        const ImVec2 text = ImGui::CalcTextSize(name.data(), name.data() + name.size());
        const float box = px(20);
        const float gap = name.empty() ? 0.0f : px(4);
        ImRect bb;
        if (!place(id, ImVec2(box + gap + text.x, box), &bb)) {
            return false;
        }
        const bool on = enabled_now(true);
        bool hovered = false;
        bool held = false;
        const bool pressed = on && ImGui::ButtonBehavior(bb, id, &hovered, &held);
        if (pressed) {
            *value = !*value;
            ImGui::MarkItemEdited(id);
        }
        // No highlight (the user's decision): only the tick comes and goes.
        const Sprite sprite = *value ? Sprite::checkbox_selected : Sprite::checkbox;
        draw_sprite(window->DrawList, sprite, bb.Min, ImVec2(bb.Min.x + box, bb.Min.y + box));
        if (!name.empty()) {
            bool shadow = false;
            const ImU32 color = on ? label_color(&shadow) : kDisabledText;
            draw_text(window->DrawList, ImVec2(bb.Min.x + box + gap, bb.Min.y + (box - text.y) * 0.5f), color, name,
                      shadow);
        }
        return pressed;
    }

    bool slider_int(const char* label, int* value, int min, int max, const char* format, int width)
    {
        ImGuiWindow* window = ImGui::GetCurrentWindow();
        if (window->SkipItems) {
            return false;
        }
        const ImGuiID id = window->GetID(label);
        ImRect bb;
        if (!place(id, ImVec2(width_of(width), px(20)), &bb)) {
            return false;
        }
        const bool on = enabled_now(true);
        bool hovered = false;
        bool held = false;
        if (on) {
            ImGui::ButtonBehavior(bb, id, &hovered, &held);
        }
        const float handle = px(8);
        const float travel = std::max(1.0f, bb.GetWidth() - handle);
        bool changed = false;
        if (held && max > min) {
            const float t = std::clamp((ImGui::GetIO().MousePos.x - bb.Min.x - handle * 0.5f) / travel, 0.0f, 1.0f);
            const int v = min + static_cast<int>(std::lround(t * static_cast<float>(max - min)));
            if (v != *value) {
                *value = v;
                changed = true;
                ImGui::MarkItemEdited(id);
            }
        }
        const float t = max > min ? static_cast<float>(std::clamp(*value, min, max) - min) /
                                        static_cast<float>(max - min)
                                  : 0.0f;
        const float x = std::floor((bb.Min.x + travel * t) / static_cast<float>(g_scale)) * static_cast<float>(g_scale);
        // Only the handle lights up while held (the user's decision), not the rail.
        draw_sprite(window->DrawList, Sprite::slider, bb.Min, bb.Max);
        draw_sprite(window->DrawList, held ? Sprite::slider_handle_highlighted : Sprite::slider_handle,
                    ImVec2(x, bb.Min.y), ImVec2(x + handle, bb.Max.y));
        char number[64];
        std::snprintf(number, sizeof number, format, *value);
        const std::string_view name = visible(label);
        const std::string shown = name.empty() ? std::string(number) : std::string(name) + ": " + number;
        centred(window->DrawList, bb.Min, bb.Max, on ? kWhite : kDisabledText, shown);
        return changed;
    }

    bool tab(const char* label, bool selected, int width)
    {
        ImGuiWindow* window = ImGui::GetCurrentWindow();
        if (window->SkipItems) {
            return false;
        }
        const ImGuiID id = window->GetID(label);
        const std::string_view name = visible(label);
        float w = px(width);
        if (width <= 0) {
            const float text = ImGui::CalcTextSize(name.data(), name.data() + name.size()).x + px(16);
            w = std::ceil(text / static_cast<float>(g_scale)) * static_cast<float>(g_scale);
        }
        ImRect bb;
        if (!place(id, ImVec2(w, px(24)), &bb)) {
            return false;
        }
        bool hovered = false;
        bool held = false;
        const bool pressed = enabled_now(true) && ImGui::ButtonBehavior(bb, id, &hovered, &held);
        const bool lit = held && hovered;
        const Sprite sprite = selected ? (lit ? Sprite::tab_selected_highlighted : Sprite::tab_selected)
                                       : (lit ? Sprite::tab_highlighted : Sprite::tab);
        draw_sprite(window->DrawList, sprite, bb.Min, bb.Max);
        // The selected tab's sprite is see-through: its text is the window's (dark on a panel, light with a shadow on
        // the dark windows); the others' are dark, their text light grey.
        bool shadow = true;
        const ImU32 color = selected ? label_color(&shadow) : kDisabledText;
        const ImVec2 size = ImGui::CalcTextSize(name.data(), name.data() + name.size());
        const ImVec2 at = snap(ImVec2((bb.Min.x + bb.Max.x - size.x) * 0.5f, (bb.Min.y + bb.Max.y - size.y) * 0.5f));
        window->DrawList->PushClipRect(bb.Min, bb.Max, true);
        draw_text(window->DrawList, at, color, name, shadow);
        window->DrawList->PopClipRect();
        return pressed;
    }

    bool begin_list(const char* id, int height)
    {
        ImGuiWindow* window = ImGui::GetCurrentWindow();
        const ImVec2 at = snap(ImGui::GetCursorScreenPos());
        const ImVec2 size(width_of(0), px(height));
        const ImVec2 end = add(at, size);
        draw_sprite(window->DrawList, Sprite::inset, at, end);
        ListFrame frame{ at, end, ImHashStr("scroller", 0, window->GetID(id)) };
        g_lists.push_back(frame);
        // The contents inside the inset's 1 px, left of the scroller (6 px) and a pixel of air.
        ImGui::SetCursorScreenPos(ImVec2(at.x + px(1), at.y + px(1)));
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(px(3), px(2)));
        const bool open = ImGui::BeginChild(id, ImVec2(size.x - px(2) - px(7), size.y - px(2)), ImGuiChildFlags_None,
                                            ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoBackground);
        ImGui::PopStyleVar();
        return open;
    }

    void end_list()
    {
        ImGuiWindow* inner = ImGui::GetCurrentWindow();
        const float scroll = inner->Scroll.y;
        const float most = inner->ScrollMax.y;
        ImGui::EndChild();
        const ListFrame frame = g_lists.back();
        g_lists.pop_back();
        ImGuiWindow* window = ImGui::GetCurrentWindow();
        const ImRect track(ImVec2(frame.max.x - px(1) - px(6), frame.min.y + px(1)),
                           ImVec2(frame.max.x - px(1), frame.max.y - px(1)));
        draw_sprite(window->DrawList, Sprite::scroller_background, track.Min, track.Max);
        if (most > 0.0f && ImGui::ItemAdd(track, frame.scroller)) {
            // The handle as Minecraft's lists: track² / content high, at least 32 GUI px (or the track).
            const float h = track.GetHeight();
            const float handle = std::clamp(std::floor(h * h / (h + most)), std::min(px(32), h), h);
            bool hovered = false;
            bool held = false;
            const bool pressed =
                ImGui::ButtonBehavior(track, frame.scroller, &hovered, &held, ImGuiButtonFlags_PressedOnClick);
            float top = track.Min.y + (h - handle) * scroll / most;
            const float mouse = ImGui::GetIO().MousePos.y;
            ImGuiStorage* storage = ImGui::GetStateStorage();
            if (pressed) {
                // Grabbed where it was clicked, or by its middle when the click was beside it.
                storage->SetFloat(frame.scroller, mouse >= top && mouse < top + handle ? mouse - top : handle * 0.5f);
            }
            if (held && h > handle) {
                const float grab = storage->GetFloat(frame.scroller, handle * 0.5f);
                const float t = std::clamp((mouse - grab - track.Min.y) / (h - handle), 0.0f, 1.0f);
                ImGui::SetScrollY(inner, t * most);
                top = track.Min.y + (h - handle) * t;
            }
            top = std::floor(top / static_cast<float>(g_scale)) * static_cast<float>(g_scale);
            draw_sprite(window->DrawList, Sprite::scroller, ImVec2(track.Min.x, top),
                        ImVec2(track.Max.x, top + handle));
        }
        ImGui::SetCursorScreenPos(frame.min);
        ImGui::Dummy(ImVec2(frame.max.x - frame.min.x, frame.max.y - frame.min.y));
    }

    bool begin_tooltip()
    {
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(px(4), px(4)));
        ImGui::PushStyleVar(ImGuiStyleVar_PopupBorderSize, 0.0f);
        ImGui::PushStyleColor(ImGuiCol_PopupBg, IM_COL32(0, 0, 0, 0));
        const bool open = ImGui::BeginTooltip();
        ImGui::PopStyleColor();
        ImGui::PopStyleVar(2);
        if (open) {
            ImDrawList* list = ImGui::GetWindowDrawList();
            list->ChannelsSplit(2);
            list->ChannelsSetCurrent(1);
        }
        return open;
    }

    void end_tooltip()
    {
        ImDrawList* list = ImGui::GetWindowDrawList();
        list->ChannelsSetCurrent(0);
        // Minecraft's tooltip sprites hold 9 GUI px of air around the box: drawn that much bigger than the window.
        const ImVec2 pos = snap(ImGui::GetWindowPos());
        const ImVec2 size = snap(ImGui::GetWindowSize());
        const ImVec2 a(pos.x - px(9), pos.y - px(9));
        const ImVec2 b(pos.x + size.x + px(9), pos.y + size.y + px(9));
        list->PushClipRectFullScreen();
        draw_sprite(list, Sprite::tooltip_background, a, b);
        draw_sprite(list, Sprite::tooltip_frame, a, b);
        list->PopClipRect();
        list->ChannelsMerge();
        ImGui::EndTooltip();
    }

    void item_icon(std::string_view item, int size)
    {
        if (ImGui::GetCurrentWindow()->SkipItems) {
            return;
        }
        const ImVec2 at = snap(ImGui::GetCursorScreenPos());
        const ImVec2 end(at.x + px(size), at.y + px(size));
        ImGui::Dummy(ImVec2(px(size), px(size)));
        if (!ImGui::IsItemVisible() || g_sprites == nullptr) {
            return;
        }
        if (const SpriteImage* icon = g_sprites->item(item)) {
            draw_sprite(ImGui::GetWindowDrawList(), *icon, at, end);
        }
    }

    bool image_button(const char* id, const SpriteImage* image, const ImVec2& size)
    {
        ImGuiWindow* window = ImGui::GetCurrentWindow();
        if (window->SkipItems) {
            return false;
        }
        const ImGuiID item = window->GetID(id);
        ImRect bb;
        if (!place(item, size, &bb)) {
            return false;
        }
        bool hovered = false;
        bool held = false;
        const bool pressed = ImGui::ButtonBehavior(bb, item, &hovered, &held);
        if (image != nullptr) {
            draw_sprite(window->DrawList, *image, bb.Min, bb.Max);
        }
        return pressed;
    }

    void slot(std::string_view item, int count)
    {
        ImGuiWindow* window = ImGui::GetCurrentWindow();
        if (window->SkipItems) {
            return;
        }
        const ImVec2 at = snap(ImGui::GetCursorScreenPos());
        const float size = px(18);
        ImGui::Dummy(ImVec2(size, size));
        if (!ImGui::IsItemVisible()) {
            return;
        }
        ImDrawList* list = window->DrawList;
        draw_sprite(list, Sprite::slot, at, ImVec2(at.x + size, at.y + size));
        if (!item.empty() && g_sprites != nullptr) {
            if (const SpriteImage* icon = g_sprites->item(item)) {
                draw_sprite(list, *icon, ImVec2(at.x + px(1), at.y + px(1)), ImVec2(at.x + px(17), at.y + px(17)));
            }
        }
        if (count > 0) {
            const std::string number = std::to_string(count);
            const ImVec2 text = ImGui::CalcTextSize(number.c_str());
            draw_text(list, ImVec2(at.x + size - text.x - px(1), at.y + size - text.y + px(1)), kWhite, number, true);
        }
    }

    void bar(float fraction, BarColor color, int width)
    {
        ImGuiWindow* window = ImGui::GetCurrentWindow();
        if (window->SkipItems) {
            return;
        }
        const ImVec2 at = snap(ImGui::GetCursorScreenPos());
        const ImVec2 end(at.x + width_of(width), at.y + px(5));
        ImGui::Dummy(ImVec2(end.x - at.x, end.y - at.y));
        if (!ImGui::IsItemVisible()) {
            return;
        }
        const auto background = static_cast<Sprite>(static_cast<int>(Sprite::bar_red_background) +
                                                    2 * static_cast<int>(color));
        const auto progress = static_cast<Sprite>(static_cast<int>(background) + 1);
        ImDrawList* list = window->DrawList;
        draw_sprite(list, background, at, end);
        const float filled = std::floor((end.x - at.x) * std::clamp(fraction, 0.0f, 1.0f));
        if (filled > 0.0f) {
            list->PushClipRect(at, ImVec2(at.x + filled, end.y), true);
            draw_sprite(list, progress, at, end);
            list->PopClipRect();
        }
    }
}
