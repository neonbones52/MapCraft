#include "host/win32/window.h"

#include <format>

namespace mc::win32
{
    namespace
    {
        // The key's scan code, extended keys (arrows, right Ctrl, numpad Enter...) with the top bit (view/input.h).
        std::uint8_t scan_code(LPARAM lparam)
        {
            auto code = static_cast<std::uint8_t>((lparam >> 16) & 0x7F);
            if (lparam & (1 << 24)) {
                code |= 0x80;
            }
            return code;
        }
    }

    bool Window::create(HINSTANCE instance, const wchar_t* class_name, const wchar_t* title, int width, int height,
                        std::string* error)
    {
        SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
        WNDCLASSEXW wc{ sizeof(wc) };
        wc.lpfnWndProc = procedure;
        wc.hInstance = instance;
        wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        wc.lpszClassName = class_name;
        RegisterClassExW(&wc);
        RECT rect{ 0, 0, width, height };
        AdjustWindowRect(&rect, WS_OVERLAPPEDWINDOW, FALSE);
        hwnd_ = CreateWindowExW(0, class_name, title, WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT,
                                rect.right - rect.left, rect.bottom - rect.top, nullptr, nullptr, instance, this);
        if (!hwnd_) {
            *error = std::format("CreateWindowExW failed ({})", GetLastError());
            return false;
        }
        width_ = width;
        height_ = height;
        input_.cursor_x = -1.0f;
        input_.cursor_y = -1.0f;
        return true;
    }

    void Window::pump()
    {
        MSG msg;
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
    }

    void Window::end_frame()
    {
        input_.wheel = 0.0f;
        input_.pressed = input_.released = 0;
        input_.keys_hit.reset();
    }

    LRESULT CALLBACK Window::procedure(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam)
    {
        if (message == WM_NCCREATE) {
            const auto* create = reinterpret_cast<const CREATESTRUCTW*>(lparam);
            SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(create->lpCreateParams));
        }
        auto* window = reinterpret_cast<Window*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
        if (!window) {
            return DefWindowProcW(hwnd, message, wparam, lparam);
        }
        if (window->hwnd_ == nullptr) {
            window->hwnd_ = hwnd;   // messages sent during CreateWindowExW
        }
        if (window->hook_ && window->hook_(hwnd, message, wparam, lparam)) {
            return TRUE;
        }
        return window->handle(message, wparam, lparam);
    }

    void Window::set_button(std::uint32_t button, bool down)
    {
        if (down && !(input_.buttons & button)) {
            input_.pressed |= button;
            input_.buttons |= button;
            SetCapture(hwnd_);
        } else if (!down && (input_.buttons & button)) {
            input_.released |= button;
            input_.buttons &= ~button;
            if (!input_.buttons) {
                ReleaseCapture();
            }
        }
    }

    LRESULT Window::handle(UINT message, WPARAM wparam, LPARAM lparam)
    {
        switch (message) {
        case WM_SIZE:
            width_ = LOWORD(lparam);
            height_ = HIWORD(lparam);
            return 0;
        case WM_MOUSEMOVE:
            input_.cursor_x = static_cast<float>(static_cast<short>(LOWORD(lparam)));
            input_.cursor_y = static_cast<float>(static_cast<short>(HIWORD(lparam)));
            if (!mouse_inside_) {
                TRACKMOUSEEVENT track{ sizeof(track), TME_LEAVE, hwnd_, 0 };
                TrackMouseEvent(&track);
                mouse_inside_ = true;
            }
            return 0;
        case WM_MOUSELEAVE:
            mouse_inside_ = false;
            input_.cursor_x = input_.cursor_y = -1.0f;   // off the window: nothing hovered
            return 0;
        case WM_MOUSEWHEEL:
            input_.wheel += static_cast<float>(GET_WHEEL_DELTA_WPARAM(wparam)) / WHEEL_DELTA;
            return 0;
        case WM_LBUTTONDOWN: set_button(mouse_left, true); return 0;
        case WM_LBUTTONUP: set_button(mouse_left, false); return 0;
        case WM_RBUTTONDOWN: set_button(mouse_right, true); return 0;
        case WM_RBUTTONUP: set_button(mouse_right, false); return 0;
        case WM_MBUTTONDOWN: set_button(mouse_middle, true); return 0;
        case WM_MBUTTONUP: set_button(mouse_middle, false); return 0;
        case WM_KEYDOWN:
        case WM_SYSKEYDOWN: {
            const std::uint8_t code = scan_code(lparam);
            if (!input_.keys[code]) {
                input_.keys_hit[code] = true;
            }
            input_.keys[code] = true;
            if (message == WM_SYSKEYDOWN) {
                break;   // Alt+F4 and the system menu keep working
            }
            return 0;
        }
        case WM_KEYUP:
        case WM_SYSKEYUP:
            input_.keys[scan_code(lparam)] = false;
            if (message == WM_SYSKEYUP) {
                break;
            }
            return 0;
        case WM_KILLFOCUS:
            input_.keys.reset();
            input_.buttons = 0;
            return 0;
        case WM_CLOSE:
            close_requested_ = true;
            return 0;
        default:
            break;
        }
        return DefWindowProcW(hwnd_, message, wparam, lparam);
    }
}
