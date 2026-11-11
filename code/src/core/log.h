#pragma once

#include <format>
#include <string_view>

namespace mc::log
{
    enum class Level
    {
        info,
        warn,
        error,
    };

    using Sink = void (*)(Level, std::string_view);

    // The host routes messages to its own log (a log file next to the exe, debugger output, ...).
    void set_sink(Sink sink);
    void write(Level level, std::string_view message);

    template <class... Args>
    void info(std::format_string<Args...> fmt, Args&&... args)
    {
        write(Level::info, std::format(fmt, std::forward<Args>(args)...));
    }

    template <class... Args>
    void warn(std::format_string<Args...> fmt, Args&&... args)
    {
        write(Level::warn, std::format(fmt, std::forward<Args>(args)...));
    }

    template <class... Args>
    void error(std::format_string<Args...> fmt, Args&&... args)
    {
        write(Level::error, std::format(fmt, std::forward<Args>(args)...));
    }
}
