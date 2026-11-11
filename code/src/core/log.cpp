#include "core/log.h"

#include <atomic>

namespace mc::log
{
    namespace
    {
        std::atomic<Sink> g_sink = nullptr;
    }

    void set_sink(Sink sink)
    {
        g_sink.store(sink);
    }

    void write(Level level, std::string_view message)
    {
        if (const Sink sink = g_sink.load()) {
            sink(level, message);
        }
    }
}
