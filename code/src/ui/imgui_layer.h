#pragma once

#include <filesystem>

#include <d3d11.h>

#include "view/input.h"

struct ImGuiContext;
struct ImFont;

namespace mc
{
    // Dear ImGui for the game in every host: fed from mc::Input (the host's window messages, translated), drawn by the
    // DX11 backend into the frame's target. Sizes follow the viewport height (1 at 1080 px and below). The font is
    // ChakraPetch (the user's decision: readable, and in the style; `fonts` holds the .ttf files, README "Ассеты"),
    // Medium for text and Bold for the army plates; Segoe UI or ImGui's own if the files are missing.
    class ImGuiLayer
    {
    public:
        ImGuiLayer() = default;
        ImGuiLayer(const ImGuiLayer&) = delete;
        ImGuiLayer& operator=(const ImGuiLayer&) = delete;
        ~ImGuiLayer();

        bool init(ID3D11Device* device, ID3D11DeviceContext* context, const std::filesystem::path& fonts);
        // ImGui::NewFrame with this frame's input; build windows until render().
        void begin(const Input& input, float view_width, float view_height, float dt);
        void render(ID3D11DeviceContext* context, ID3D11RenderTargetView* target);

        // The cursor is over a window (or a window holds the mouse): the map gets no mouse this frame.
        [[nodiscard]] bool wants_mouse() const;
        [[nodiscard]] float scale() const { return scale_; }
        [[nodiscard]] ImFont* bold() const { return bold_; }   // never null after init

    private:
        ImGuiContext* context_ = nullptr;
        bool backend_ = false;
        float scale_ = 0.0f;
        ImFont* bold_ = nullptr;
    };
}
