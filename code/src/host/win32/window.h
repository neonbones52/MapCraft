#pragma once

#include <functional>
#include <string>

#include <Windows.h>

#include "view/input.h"

namespace mc::win32
{
    // The Win32 window of mapcraft.exe and mapcraft_editor.exe: the class, per-monitor DPI awareness, the message
    // loop, and the messages turned into mc::Input (cursor, wheel, buttons with capture, keys by scan code; focus loss
    // releases everything, leaving the window clears the cursor).
    class Window
    {
    public:
        // Sees every message first; true = handled, the window ignores it (the editor's ImGui Win32 backend).
        using Hook = std::function<bool(HWND, UINT, WPARAM, LPARAM)>;

        // A window with a `width` x `height` px client area, hidden until show().
        bool create(HINSTANCE instance, const wchar_t* class_name, const wchar_t* title, int width, int height,
                    std::string* error);
        void set_hook(Hook hook) { hook_ = std::move(hook); }
        void show(int command) { ShowWindow(hwnd_, command); }
        void set_title(const std::wstring& title) { SetWindowTextW(hwnd_, title.c_str()); }

        // Handles the pending messages.
        void pump();
        // The frame's input is used: clears what lasts one frame (wheel, presses, releases, key hits).
        void end_frame();

        [[nodiscard]] HWND hwnd() const { return hwnd_; }
        [[nodiscard]] int width() const { return width_; }
        [[nodiscard]] int height() const { return height_; }
        [[nodiscard]] bool minimized() const { return width_ <= 0 || height_ <= 0; }
        [[nodiscard]] const Input& input() const { return input_; }
        // The close button (or Alt+F4) was pressed; the host decides, then clears it.
        [[nodiscard]] bool close_requested() const { return close_requested_; }
        void clear_close_request() { close_requested_ = false; }

    private:
        static LRESULT CALLBACK procedure(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam);
        LRESULT handle(UINT message, WPARAM wparam, LPARAM lparam);
        void set_button(std::uint32_t button, bool down);

        HWND hwnd_ = nullptr;
        int width_ = 0;
        int height_ = 0;
        bool mouse_inside_ = false;
        bool close_requested_ = false;
        Input input_;
        Hook hook_;
    };
}
