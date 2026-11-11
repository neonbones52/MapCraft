// The game's host (mapcraft.exe): a plain Win32 window with its own D3D11 swap chain.

#include <Windows.h>
#include <d3d11.h>
#include <dxgi1_2.h>
#include <wrl/client.h>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <string>

#include "app/shell.h"
#include "core/log.h"

using Microsoft::WRL::ComPtr;

namespace
{
    constexpr int kWidth = 1600;    // client area at start
    constexpr int kHeight = 900;

    struct Window
    {
        HWND hwnd = nullptr;
        int width = 0;
        int height = 0;
        bool resized = false;
        bool closing = false;
        bool mouse_inside = false;
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
            SetCapture(g_window.hwnd);
        } else if (!down && (in.buttons & button)) {
            in.released |= button;
            in.buttons &= ~button;
            if (!in.buttons) {
                ReleaseCapture();
            }
        }
    }

    LRESULT CALLBACK window_proc(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam)
    {
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
            if (!g_window.mouse_inside) {
                TRACKMOUSEEVENT track{ sizeof(track), TME_LEAVE, hwnd, 0 };
                TrackMouseEvent(&track);
                g_window.mouse_inside = true;
            }
            return 0;
        case WM_MOUSELEAVE:
            g_window.mouse_inside = false;
            in.cursor_x = in.cursor_y = -1.0f;   // off the window: nothing hovered
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
            g_window.closing = true;
            return 0;
        default:
            break;
        }
        return DefWindowProcW(hwnd, message, wparam, lparam);
    }
}

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, LPWSTR, int show)
{
    wchar_t exe[MAX_PATH];
    GetModuleFileNameW(nullptr, exe, MAX_PATH);
    g_log.open(std::filesystem::path(exe).replace_filename(L"mapcraft.log"));
    mc::log::set_sink(log_sink);

    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    WNDCLASSEXW wc{ sizeof(wc) };
    wc.lpfnWndProc = window_proc;
    wc.hInstance = instance;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.lpszClassName = L"MapCraft";
    RegisterClassExW(&wc);
    RECT rect{ 0, 0, kWidth, kHeight };
    AdjustWindowRect(&rect, WS_OVERLAPPEDWINDOW, FALSE);
    g_window.hwnd = CreateWindowExW(0, wc.lpszClassName, L"MapCraft", WS_OVERLAPPEDWINDOW,
                                    CW_USEDEFAULT, CW_USEDEFAULT, rect.right - rect.left, rect.bottom - rect.top,
                                    nullptr, nullptr, instance, nullptr);
    g_window.width = kWidth;
    g_window.height = kHeight;
    g_window.input.cursor_x = -1.0f;
    g_window.input.cursor_y = -1.0f;

    ComPtr<ID3D11Device> device;
    ComPtr<ID3D11DeviceContext> context;
    UINT flags = 0;
#ifndef NDEBUG
    flags |= D3D11_CREATE_DEVICE_DEBUG;
#endif
    const D3D_FEATURE_LEVEL level = D3D_FEATURE_LEVEL_11_0;
    if (FAILED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, flags, &level, 1, D3D11_SDK_VERSION,
                                 &device, nullptr, &context))) {
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

    mc::Shell shell;
    std::string error;
    if (!shell.app().init(device.Get(), MC_DEFAULT_MAP, &error)) {
        mc::log::error("{}", error);
        MessageBoxA(g_window.hwnd, error.c_str(), "MapCraft", MB_ICONERROR);
        return 1;
    }
    if (std::filesystem::exists(MC_DEFAULT_DATA) && !shell.load_world(MC_DEFAULT_DATA, &error)) {
        mc::log::error("{}", error);
        MessageBoxA(g_window.hwnd, error.c_str(), "MapCraft: game data", MB_ICONWARNING);
    }
    shell.init(device.Get(), context.Get(), MC_DEFAULT_FLAGS);
    ShowWindow(g_window.hwnd, show);

    ComPtr<ID3D11RenderTargetView> rtv;
    auto previous = std::chrono::steady_clock::now();
    MSG msg;
    while (!g_window.closing) {
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
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

        shell.frame(context.Get(), rtv.Get(), static_cast<float>(g_window.width), static_cast<float>(g_window.height), dt,
                    g_window.input);
        g_window.input.wheel = 0.0f;
        g_window.input.pressed = g_window.input.released = 0;
        g_window.input.keys_hit.reset();

        swap_chain->Present(1, 0);
        if (shell.exit_requested()) {
            break;
        }
    }
    return 0;
}
