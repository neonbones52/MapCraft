// Province editor: the map as in mapcraft.exe, with Dear ImGui panels to set each province's owner, controller, cores,
// terrain, culture, religion, development and trade; saves data/setup (docs/GAMEDATA.md).
//   mapcraft_editor [--check]
//   --check: load the data, log errors and warnings (mapcraft_editor.log), exit 1 on an error; no window.

#include <Windows.h>
#include <shellapi.h>
#include <d3d11.h>
#include <dxgi1_2.h>
#include <wrl/client.h>

#include <chrono>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <string>

#include <imgui.h>
#include <imgui_impl_dx11.h>
#include <imgui_impl_win32.h>

#include "app/app.h"
#include "app/bindings.h"
#include "core/log.h"
#include "minecraft/assets.h"
#include "editor/editor.h"

extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam);

using Microsoft::WRL::ComPtr;

namespace
{
    struct Window
    {
        HWND hwnd = nullptr;
        int width = 0;
        int height = 0;
        bool resized = false;
        bool close_requested = false;
        mc::Input input;
    };

    Window g_window;
    std::ofstream g_log;

    void log_sink(mc::log::Level level, std::string_view message)
    {
        static constexpr const char* names[] = { "info", "warn", "error" };
        std::string line = std::string("[") + names[static_cast<int>(level)] + "] " + std::string(message) + "\n";
        OutputDebugStringA(line.c_str());
        if (g_log) {
            g_log << line << std::flush;
        }
    }

    std::uint8_t scan_code(LPARAM lparam)
    {
        auto code = static_cast<std::uint8_t>((lparam >> 16) & 0x7F);
        if (lparam & (1 << 24)) {
            code |= 0x80;
        }
        return code;
    }

    void set_button(std::uint32_t button, bool down)
    {
        mc::Input& in = g_window.input;
        if (down && !(in.buttons & button)) {
            in.pressed |= button;
            in.buttons |= button;
        } else if (!down && (in.buttons & button)) {
            in.released |= button;
            in.buttons &= ~button;
        }
    }

    LRESULT CALLBACK window_proc(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam)
    {
        if (ImGui_ImplWin32_WndProcHandler(hwnd, message, wparam, lparam)) {
            return TRUE;
        }
        mc::Input& in = g_window.input;
        switch (message) {
        case WM_SIZE:
            g_window.width = LOWORD(lparam);
            g_window.height = HIWORD(lparam);
            g_window.resized = true;
            return 0;
        case WM_MOUSEMOVE:
            in.cursor_x = static_cast<float>(static_cast<short>(LOWORD(lparam)));
            in.cursor_y = static_cast<float>(static_cast<short>(HIWORD(lparam)));
            return 0;
        case WM_MOUSEWHEEL:
            in.wheel += static_cast<float>(GET_WHEEL_DELTA_WPARAM(wparam)) / WHEEL_DELTA;
            return 0;
        case WM_LBUTTONDOWN: set_button(mc::mouse_left, true); return 0;
        case WM_LBUTTONUP: set_button(mc::mouse_left, false); return 0;
        case WM_RBUTTONDOWN: set_button(mc::mouse_right, true); return 0;
        case WM_RBUTTONUP: set_button(mc::mouse_right, false); return 0;
        case WM_MBUTTONDOWN: set_button(mc::mouse_middle, true); return 0;
        case WM_MBUTTONUP: set_button(mc::mouse_middle, false); return 0;
        case WM_KEYDOWN:
        case WM_SYSKEYDOWN: {
            const std::uint8_t code = scan_code(lparam);
            if (!in.keys[code]) {
                in.keys_hit[code] = true;
            }
            in.keys[code] = true;
            if (message == WM_SYSKEYDOWN) {
                break;
            }
            return 0;
        }
        case WM_KEYUP:
        case WM_SYSKEYUP:
            in.keys[scan_code(lparam)] = false;
            if (message == WM_SYSKEYUP) {
                break;
            }
            return 0;
        case WM_KILLFOCUS:
            in.keys.reset();
            in.buttons = 0;
            return 0;
        case WM_CLOSE:
            g_window.close_requested = true;
            return 0;
        default:
            break;
        }
        return DefWindowProcW(hwnd, message, wparam, lparam);
    }

    void set_title(bool dirty, const std::filesystem::path& data)
    {
        const std::wstring title = std::wstring(L"MapCraft editor - ") + data.wstring() + (dirty ? L" *" : L"");
        SetWindowTextW(g_window.hwnd, title.c_str());
    }

    void setup_imgui(float dpi_scale)
    {
        ImGui::CreateContext();
        ImGuiIO& io = ImGui::GetIO();
        io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
        static std::string ini;   // window layout next to the exe
        wchar_t exe[MAX_PATH];
        GetModuleFileNameW(nullptr, exe, MAX_PATH);
        ini = std::filesystem::path(exe).replace_filename(L"mapcraft_editor.ini").string();
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
}

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, LPWSTR, int show)
{
    const std::filesystem::path map_file = MC_DEFAULT_MAP;
    const std::filesystem::path data_dir = MC_DEFAULT_DATA;
    constexpr int width = 1800;
    constexpr int height = 1000;
    bool check = false;
    {
        int argc = 0;
        wchar_t** argv = CommandLineToArgvW(GetCommandLineW(), &argc);
        for (int i = 1; i < argc; ++i) {
            check = check || std::wstring(argv[i]) == L"--check";
        }
        LocalFree(argv);
    }

    wchar_t exe[MAX_PATH];
    GetModuleFileNameW(nullptr, exe, MAX_PATH);
    g_log.open(std::filesystem::path(exe).replace_filename(L"mapcraft_editor.log"));
    mc::log::set_sink(log_sink);

    ImGui_ImplWin32_EnableDpiAwareness();
    WNDCLASSEXW wc{ sizeof(wc) };
    wc.lpfnWndProc = window_proc;
    wc.hInstance = instance;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.lpszClassName = L"MapCraftEditor";
    RegisterClassExW(&wc);
    RECT rect{ 0, 0, width, height };
    AdjustWindowRect(&rect, WS_OVERLAPPEDWINDOW, FALSE);
    g_window.hwnd = CreateWindowExW(0, wc.lpszClassName, L"MapCraft editor", WS_OVERLAPPEDWINDOW,
                                    CW_USEDEFAULT, CW_USEDEFAULT, rect.right - rect.left, rect.bottom - rect.top,
                                    nullptr, nullptr, instance, nullptr);
    g_window.width = width;
    g_window.height = height;
    g_window.input.cursor_x = -1.0f;
    g_window.input.cursor_y = -1.0f;

    ComPtr<ID3D11Device> device;
    ComPtr<ID3D11DeviceContext> context;
    const D3D_FEATURE_LEVEL level = D3D_FEATURE_LEVEL_11_0;
    if (FAILED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, &level, 1, D3D11_SDK_VERSION, &device,
                                 nullptr, &context))) {
        mc::log::error("D3D11CreateDevice failed");
        return 1;
    }
    ComPtr<IDXGIDevice> dxgi_device;
    ComPtr<IDXGIAdapter> adapter;
    ComPtr<IDXGIFactory2> factory;
    device.As(&dxgi_device);
    dxgi_device->GetAdapter(&adapter);
    adapter->GetParent(IID_PPV_ARGS(&factory));
    DXGI_SWAP_CHAIN_DESC1 sc{};
    sc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    sc.SampleDesc.Count = 1;
    sc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    sc.BufferCount = 2;
    sc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    ComPtr<IDXGISwapChain1> swap_chain;
    if (FAILED(factory->CreateSwapChainForHwnd(device.Get(), g_window.hwnd, &sc, nullptr, nullptr, &swap_chain))) {
        mc::log::error("CreateSwapChainForHwnd failed");
        return 1;
    }

    mc::GameData game;   // the start being edited; before the App that shows it
    mc::App app;
    std::string error;
    if (!app.init(device.Get(), map_file, data_dir / "common" / "biomes.csv", mc::minecraft::find_jar(), &error)) {
        mc::log::error("{}", error);
        MessageBoxA(g_window.hwnd, error.c_str(), "MapCraft editor", MB_ICONERROR);
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

    setup_imgui(ImGui_ImplWin32_GetDpiScaleForHwnd(g_window.hwnd));
    ImGui_ImplWin32_Init(g_window.hwnd);
    ImGui_ImplDX11_Init(device.Get(), context.Get());
    set_title(false, data_dir);
    ShowWindow(g_window.hwnd, show);

    ComPtr<ID3D11RenderTargetView> rtv;
    auto previous = std::chrono::steady_clock::now();
    bool was_dirty = false;
    while (true) {
        MSG msg;
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
        if (g_window.close_requested) {
            g_window.close_requested = false;
            if (!editor.dirty()) {
                break;
            }
            const int answer = MessageBoxW(g_window.hwnd, L"Save the changes before closing?",
                                           L"MapCraft editor", MB_YESNOCANCEL | MB_ICONQUESTION);
            if (answer == IDNO || (answer == IDYES && editor.save())) {
                break;
            }
        }
        if (g_window.width <= 0 || g_window.height <= 0) {
            Sleep(10);
            continue;
        }
        if (g_window.resized || !rtv) {
            rtv.Reset();
            swap_chain->ResizeBuffers(0, static_cast<UINT>(g_window.width), static_cast<UINT>(g_window.height),
                                      DXGI_FORMAT_UNKNOWN, 0);
            ComPtr<ID3D11Texture2D> back;
            swap_chain->GetBuffer(0, IID_PPV_ARGS(&back));
            device->CreateRenderTargetView(back.Get(), nullptr, &rtv);
            g_window.resized = false;
        }
        const auto now = std::chrono::steady_clock::now();
        const float dt = std::chrono::duration<float>(now - previous).count();
        previous = now;
        const float view_width = static_cast<float>(g_window.width);
        const float view_height = static_cast<float>(g_window.height);

        ImGui_ImplDX11_NewFrame();
        ImGui_ImplWin32_NewFrame();
        ImGui::NewFrame();
        const ImGuiIO& io = ImGui::GetIO();

        // The map gets the mouse only when it is not over a panel, the keyboard only when no field is typed into.
        mc::Input map_input = g_window.input;
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
        app.frame(context.Get(), rtv.Get(), view_width, view_height, dt, mc::camera_input(map_input));
        if (!io.WantCaptureMouse) {
            editor.map_input(map_input, io.KeyCtrl, io.KeyShift, io.KeyAlt);
        }
        editor.ui(view_width, view_height);

        ImGui::Render();
        ID3D11RenderTargetView* targets[] = { rtv.Get() };
        context->OMSetRenderTargets(1, targets, nullptr);
        ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());
        swap_chain->Present(1, 0);

        g_window.input.wheel = 0.0f;
        g_window.input.pressed = g_window.input.released = 0;
        g_window.input.keys_hit.reset();
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
