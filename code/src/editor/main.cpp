// Province editor: the map as in mapcraft.exe, with Dear ImGui panels to set each province's owner, controller, cores,
// terrain, good, base tax and population, and each region's trade node; saves data/setup (docs/GAMEDATA.md).
//   mapcraft_editor [--check]
//   --check: load the data, log errors and warnings (mapcraft_editor.log), exit 1 on an error; no window.

#include <chrono>
#include <cmath>
#include <filesystem>
#include <string>

#include <imgui.h>
#include <imgui_impl_dx11.h>
#include <imgui_impl_win32.h>

#include <Windows.h>
#include <shellapi.h>

#include "app/app.h"
#include "app/bindings.h"
#include "core/log.h"
#include "editor/editor.h"
#include "host/win32/d3d_target.h"
#include "host/win32/log_file.h"
#include "host/win32/window.h"
#include "minecraft/assets.h"

extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam);

namespace
{
    mc::win32::Window g_window;

    void set_title(bool dirty, const std::filesystem::path& data)
    {
        g_window.set_title(std::wstring(L"MapCraft editor - ") + data.wstring() + (dirty ? L" *" : L""));
    }

    void setup_imgui(float dpi_scale)
    {
        ImGui::CreateContext();
        ImGuiIO& io = ImGui::GetIO();
        io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
        static const std::string ini = mc::win32::beside_exe(L"mapcraft_editor.ini").string();   // window layout
        io.IniFilename = ini.c_str();
        ImGui::StyleColorsDark();
        ImGuiStyle& style = ImGui::GetStyle();
        style.WindowRounding = 4.0f;
        style.FrameRounding = 3.0f;
        style.WindowBorderSize = 1.0f;
        style.Colors[ImGuiCol_WindowBg].w = 0.94f;
        style.ScaleAllSizes(dpi_scale);
        const char* font = "C:\\Windows\\Fonts\\segoeui.ttf";
        if (std::filesystem::exists(font)) {
            io.Fonts->AddFontFromFileTTF(font, std::round(17.0f * dpi_scale), nullptr,
                                         io.Fonts->GetGlyphRangesCyrillic());
        } else {
            io.FontGlobalScale = dpi_scale;
        }
    }

    bool check_requested()
    {
        int argc = 0;
        wchar_t** argv = CommandLineToArgvW(GetCommandLineW(), &argc);
        bool check = false;
        for (int i = 1; i < argc; ++i) {
            check = check || std::wstring(argv[i]) == L"--check";
        }
        LocalFree(argv);
        return check;
    }
}

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, LPWSTR, int show)
{
    const std::filesystem::path map_file = MC_DEFAULT_MAP;
    const std::filesystem::path data_dir = MC_DEFAULT_DATA;
    constexpr int width = 1800;
    constexpr int height = 1000;
    const bool check = check_requested();
    mc::win32::open_log(L"mapcraft_editor.log");

    std::string error;
    mc::win32::D3DTarget target;
    if (!g_window.create(instance, L"MapCraftEditor", L"MapCraft editor", width, height, &error) ||
        !target.create(g_window.hwnd(), &error)) {
        mc::log::error("{}", error);
        MessageBoxA(nullptr, error.c_str(), "MapCraft editor", MB_ICONERROR);
        return 1;
    }
    g_window.set_hook([](HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam) {
        return ImGui::GetCurrentContext() && ImGui_ImplWin32_WndProcHandler(hwnd, message, wparam, lparam) != 0;
    });

    mc::GameData game;   // the start being edited; before the App that shows it
    mc::App app;
    if (!app.init(target.device(), map_file, data_dir / "common" / "biomes.csv", mc::minecraft::find_jar(), &error)) {
        mc::log::error("{}", error);
        MessageBoxA(g_window.hwnd(), error.c_str(), "MapCraft editor", MB_ICONERROR);
        return 1;
    }
    mc::Editor editor(app, game, data_dir);
    const bool loaded = game.load(data_dir, app.map(), &error);
    app.set_game(&game);
    if (loaded) {
        mc::log::info("game data {}: {}", data_dir.string(), game.summary());
        for (const std::string& w : game.warnings) {
            mc::log::warn("game data: {}", w);
        }
    } else {
        mc::log::error("{}", error);
        editor.set_message("Game data not loaded (fix the file and restart):\n" + error);
    }
    if (check) {
        mc::log::info("check: {}", loaded ? "ok" : "failed");
        return loaded ? 0 : 1;
    }
    app.set_map_mode(mc::MapMode::political);

    setup_imgui(ImGui_ImplWin32_GetDpiScaleForHwnd(g_window.hwnd()));
    ImGui_ImplWin32_Init(g_window.hwnd());
    ImGui_ImplDX11_Init(target.device(), target.context());
    set_title(false, data_dir);
    g_window.show(show);

    auto previous = std::chrono::steady_clock::now();
    bool was_dirty = false;
    while (true) {
        g_window.pump();
        if (g_window.close_requested()) {
            g_window.clear_close_request();
            if (!editor.dirty()) {
                break;
            }
            const int answer = MessageBoxW(g_window.hwnd(), L"Save the changes before closing?",
                                           L"MapCraft editor", MB_YESNOCANCEL | MB_ICONQUESTION);
            if (answer == IDNO || (answer == IDYES && editor.save())) {
                break;
            }
        }
        if (g_window.minimized()) {
            Sleep(10);
            continue;
        }
        ID3D11RenderTargetView* view = target.begin_frame(g_window.width(), g_window.height());
        const auto now = std::chrono::steady_clock::now();
        const float dt = std::chrono::duration<float>(now - previous).count();
        previous = now;
        const float view_width = static_cast<float>(g_window.width());
        const float view_height = static_cast<float>(g_window.height());

        ImGui_ImplDX11_NewFrame();
        ImGui_ImplWin32_NewFrame();
        ImGui::NewFrame();
        const ImGuiIO& io = ImGui::GetIO();

        // The map gets the mouse only when it is not over a panel, the keyboard only when no field is typed into.
        mc::Input map_input = g_window.input();
        if (io.WantCaptureMouse) {
            map_input.cursor_x = map_input.cursor_y = -1.0f;
            map_input.wheel = 0.0f;
            map_input.pressed = 0;
            map_input.buttons &= ~mc::mouse_left;
        }
        if (io.WantCaptureKeyboard || io.KeyCtrl) {
            map_input.keys.reset();
            map_input.keys_hit.reset();
        }

        if (const auto mode = mc::map_mode_key(map_input)) {
            app.set_map_mode(*mode);
        }
        app.frame(target.context(), view, view_width, view_height, dt, mc::camera_input(map_input));
        if (!io.WantCaptureMouse) {
            editor.map_input(map_input, io.KeyCtrl, io.KeyShift, io.KeyAlt);
        }
        editor.ui(view_width, view_height);

        ImGui::Render();
        ID3D11RenderTargetView* targets[] = { view };
        target.context()->OMSetRenderTargets(1, targets, nullptr);
        ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());
        target.present();

        g_window.end_frame();
        if (editor.dirty() != was_dirty) {
            was_dirty = editor.dirty();
            set_title(was_dirty, data_dir);
        }
    }
    ImGui_ImplDX11_Shutdown();
    ImGui_ImplWin32_Shutdown();
    ImGui::DestroyContext();
    return 0;
}
