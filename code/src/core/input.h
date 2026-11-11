#pragma once

#include <bitset>
#include <cstdint>

namespace mc
{
    // Keys are set-1 scan codes, extended keys with bit 7 set: the Win32 scan codes the host reports (identical to
    // DirectInput DIK_* codes).
    namespace key
    {
        inline constexpr std::uint8_t escape = 0x01;
        inline constexpr std::uint8_t digit1 = 0x02;
        inline constexpr std::uint8_t digit2 = 0x03;
        inline constexpr std::uint8_t digit3 = 0x04;
        inline constexpr std::uint8_t digit4 = 0x05;
        inline constexpr std::uint8_t minus = 0x0C;
        inline constexpr std::uint8_t equals = 0x0D;
        inline constexpr std::uint8_t w = 0x11;
        inline constexpr std::uint8_t a = 0x1E;
        inline constexpr std::uint8_t s = 0x1F;
        inline constexpr std::uint8_t d = 0x20;
        inline constexpr std::uint8_t space = 0x39;
        inline constexpr std::uint8_t numpad_minus = 0x4A;
        inline constexpr std::uint8_t numpad_plus = 0x4E;
        inline constexpr std::uint8_t home = 0xC7;
        inline constexpr std::uint8_t up = 0xC8;
        inline constexpr std::uint8_t left = 0xCB;
        inline constexpr std::uint8_t right = 0xCD;
        inline constexpr std::uint8_t down = 0xD0;
    }

    enum MouseButton : std::uint32_t
    {
        mouse_left = 1u << 0,
        mouse_right = 1u << 1,
        mouse_middle = 1u << 2,
    };

    // One frame of input, already translated by the host.
    struct Input
    {
        float cursor_x = 0.0f;  // client-area pixels
        float cursor_y = 0.0f;
        float wheel = 0.0f;     // notches this frame, positive = away from the user (zoom in)
        std::uint32_t buttons = 0;   // MouseButton bits held
        std::uint32_t pressed = 0;   // MouseButton bits that went down this frame
        std::uint32_t released = 0;  // MouseButton bits that went up this frame
        std::bitset<256> keys;       // held
        std::bitset<256> keys_hit;   // went down this frame

        [[nodiscard]] bool held(std::uint8_t k) const { return keys[k]; }
        [[nodiscard]] bool hit(std::uint8_t k) const { return keys_hit[k]; }
    };
}
