#include "host/win32/log_file.h"

#include <fstream>
#include <string>

#include <Windows.h>

#include "core/log.h"

namespace mc::win32
{
    namespace
    {
        std::ofstream g_log;

        void sink(log::Level level, std::string_view message)
        {
            static constexpr const char* names[] = { "info", "warn", "error" };
            const std::string line =
                std::string("[") + names[static_cast<int>(level)] + "] " + std::string(message) + "\n";
            OutputDebugStringA(line.c_str());
            if (g_log) {
                g_log << line << std::flush;
            }
        }
    }

    std::filesystem::path beside_exe(const wchar_t* name)
    {
        wchar_t exe[MAX_PATH];
        GetModuleFileNameW(nullptr, exe, MAX_PATH);
        return std::filesystem::path(exe).replace_filename(name);
    }

    void open_log(const wchar_t* name)
    {
        g_log.open(beside_exe(name));
        log::set_sink(sink);
    }
}
