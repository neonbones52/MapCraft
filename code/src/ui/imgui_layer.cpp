#include "ui/imgui_layer.h"

#include <algorithm>
#include <cfloat>
#include <cmath>
#include <filesystem>

#include <imgui.h>
#include <imgui_impl_dx11.h>

namespace mc
{
    namespace
    {
        constexpr float kFontSize = 18.0f;            // px at scale 1
        constexpr float kReferenceHeight = 1080.0f;   // viewport height of scale 1
        ImGuiStyle g_base_style;                      // unscaled, re-applied when the scale changes

        void set_style()
        {
            ImGui::StyleColorsDark();
            ImGuiStyle& style = ImGui::GetStyle();
            style.WindowRounding = 4.0f;
            style.FrameRounding = 3.0f;
            style.WindowBorderSize = 1.0f;
            style.WindowPadding = ImVec2(12.0f, 10.0f);
            style.ItemSpacing = ImVec2(8.0f, 6.0f);
            style.Colors[ImGuiCol_WindowBg] = ImVec4(0.06f, 0.08f, 0.12f, 0.92f);
            style.Colors[ImGuiCol_Border] = ImVec4(0.45f, 0.40f, 0.30f, 0.60f);
            g_base_style = style;
        }
    }

    ImGuiLayer::~ImGuiLayer()
    {
        if (!context_) {
            return;
        }
        ImGui::SetCurrentContext(context_);
        if (backend_) {
            ImGui_ImplDX11_Shutdown();
        }
        ImGui::DestroyContext(context_);
    }

    bool ImGuiLayer::init(ID3D11Device* device, ID3D11DeviceContext* context)
    {
        context_ = ImGui::CreateContext();
        ImGui::SetCurrentContext(context_);
        ImGuiIO& io = ImGui::GetIO();
        io.IniFilename = nullptr;    // no imgui.ini next to the exe; the layout is fixed by the game anyway
        io.LogFilename = nullptr;
        io.ConfigFlags |= ImGuiConfigFlags_NoMouseCursorChange;   // the host owns the OS cursor
        io.MouseDrawCursor = false;
        set_style();
        const char* font = "C:\\Windows\\Fonts\\segoeui.ttf";
        if (std::filesystem::exists(font)) {
            io.Fonts->AddFontFromFileTTF(font, kFontSize);
        } else {
            io.Fonts->AddFontDefault();
        }
        ImGui::GetStyle().FontSizeBase = kFontSize;
        g_base_style.FontSizeBase = kFontSize;
        backend_ = ImGui_ImplDX11_Init(device, context);
        return backend_;
    }

    void ImGuiLayer::begin(const Input& input, float view_width, float view_height, float dt)
    {
        ImGui::SetCurrentContext(context_);
        const float scale = std::clamp(std::round(view_height / kReferenceHeight * 8.0f) / 8.0f, 1.0f, 3.0f);
        if (scale != scale_) {
            scale_ = scale;
            ImGuiStyle& style = ImGui::GetStyle();
            style = g_base_style;
            style.ScaleAllSizes(scale);
            style.FontScaleDpi = scale;
        }
        ImGuiIO& io = ImGui::GetIO();
        io.DisplaySize = ImVec2(view_width, view_height);
        io.DeltaTime = std::max(dt, 1e-4f);
        if (input.cursor_x >= 0.0f && input.cursor_y >= 0.0f) {
            io.AddMousePosEvent(input.cursor_x, input.cursor_y);
        } else {
            io.AddMousePosEvent(-FLT_MAX, -FLT_MAX);
        }
        // Down before up: a click within one frame still reaches ImGui (its input queue spreads them over frames).
        constexpr std::uint32_t buttons[] = { mouse_left, mouse_right, mouse_middle };
        for (int i = 0; i < 3; ++i) {
            if (input.pressed & buttons[i]) {
                io.AddMouseButtonEvent(i, true);
            }
            if (input.released & buttons[i]) {
                io.AddMouseButtonEvent(i, false);
            }
        }
        if (input.wheel != 0.0f) {
            io.AddMouseWheelEvent(0.0f, input.wheel);
        }
        ImGui_ImplDX11_NewFrame();
        ImGui::NewFrame();
    }

    void ImGuiLayer::render(ID3D11DeviceContext* context, ID3D11RenderTargetView* target)
    {
        ImGui::SetCurrentContext(context_);
        ImGui::Render();
        context->OMSetRenderTargets(1, &target, nullptr);
        ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());
        ID3D11RenderTargetView* no_target = nullptr;
        context->OMSetRenderTargets(1, &no_target, nullptr);
    }

    bool ImGuiLayer::wants_mouse() const
    {
        ImGui::SetCurrentContext(context_);
        return ImGui::GetIO().WantCaptureMouse;
    }
}
