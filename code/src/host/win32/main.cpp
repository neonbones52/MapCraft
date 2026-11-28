// The game's host (mapcraft.exe): the Win32 window and its D3D11 target (host/win32), the Shell's frame in it.

#include <chrono>
#include <filesystem>
#include <string>

#include "app/shell.h"
#include "core/log.h"
#include "host/win32/d3d_target.h"
#include "host/win32/log_file.h"
#include "host/win32/window.h"
#include "minecraft/assets.h"

namespace
{
    constexpr int kWidth = 1600;    // client area at start
    constexpr int kHeight = 900;

    int fail(HWND hwnd, const std::string& error)
    {
        mc::log::error("{}", error);
        MessageBoxA(hwnd, error.c_str(), "MapCraft", MB_ICONERROR);
        return 1;
    }
}

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, LPWSTR, int show)
{
    mc::win32::open_log(L"mapcraft.log");
    std::string error;
    mc::win32::Window window;
    mc::win32::D3DTarget target;
    if (!window.create(instance, L"MapCraft", L"MapCraft", kWidth, kHeight, &error) ||
        !target.create(window.hwnd(), &error)) {
        return fail(nullptr, error);
    }

    mc::Shell shell;
    const std::filesystem::path jar = mc::minecraft::find_jar();
    if (!shell.app().init(target.device(), MC_DEFAULT_MAP,
                          std::filesystem::path(MC_DEFAULT_DATA) / "common" / "biomes.csv", jar, &error)) {
        return fail(window.hwnd(), error);
    }
    if (std::filesystem::exists(MC_DEFAULT_DATA) && !shell.load_world(MC_DEFAULT_DATA, &error)) {
        mc::log::error("{}", error);
        MessageBoxA(window.hwnd(), error.c_str(), "MapCraft: game data", MB_ICONWARNING);
    }
    shell.init(target.device(), target.context(), MC_DEFAULT_FLAGS, MC_DEFAULT_FONTS, jar);
    window.show(show);

    auto previous = std::chrono::steady_clock::now();
    while (!window.close_requested() && !shell.exit_requested()) {
        window.pump();
        if (window.minimized()) {
            Sleep(10);
            continue;
        }
        ID3D11RenderTargetView* view = target.begin_frame(window.width(), window.height());
        const auto now = std::chrono::steady_clock::now();
        const float dt = std::chrono::duration<float>(now - previous).count();
        previous = now;

        shell.frame(target.context(), view, static_cast<float>(window.width()), static_cast<float>(window.height()), dt,
                    window.input());
        window.end_frame();
        target.present();
    }
    return 0;
}
