#pragma once

#include <filesystem>

namespace mc::win32
{
    // `name` next to the running exe ("mapcraft.log", "mapcraft_editor.ini").
    [[nodiscard]] std::filesystem::path beside_exe(const wchar_t* name);
    // The log (core/log.h) goes to the debugger and to `name` beside the exe, rewritten at each start.
    void open_log(const wchar_t* name);
}
