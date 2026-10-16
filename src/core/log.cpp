#include "core/log.h"

#include <atomic>
#include <cstdarg>
#include <cstdio>

extern "C" void (*TSL_PlatformLogHook)(int warning, const char *message);

namespace tsl {
namespace {

void DefaultSink(IHS_LogLevel level, const char *tag, const char *message) {
    std::fprintf(stderr, "[%s %s] %s\n", IHS_LogLevelName(level), tag, message);
}

std::atomic<LogSink> g_sink{DefaultSink};
std::atomic<int> g_maxLevel{IHS_LogLevelInfo};

void IhsTrampoline(IHS_LogLevel level, const char *tag, const char *message) {
    if (level > g_maxLevel.load()) return;
    LogSink sink = g_sink.load();
    if (sink != nullptr) sink(level, tag, message);
}

// UDP backend messages (src/platform/ihs_udp_poll.c) go to the same sink.
void PlatformLog(int warning, const char *message) {
    IhsTrampoline(warning ? IHS_LogLevelWarn : IHS_LogLevelInfo, "UDP", message);
}

struct PlatformLogInstaller {
    PlatformLogInstaller() { TSL_PlatformLogHook = PlatformLog; }
} g_platformLogInstaller;

}  // namespace

void SetLogSink(LogSink sink) { g_sink.store(sink != nullptr ? sink : DefaultSink); }

void SetLogLevel(IHS_LogLevel maxLevel) { g_maxLevel.store(maxLevel); }

IHS_LogFunction *IhsLogFunction() { return IhsTrampoline; }

void Log(IHS_LogLevel level, const char *tag, const char *fmt, ...) {
    if (level > g_maxLevel.load()) return;
    char buf[1024];
    va_list args;
    va_start(args, fmt);
    std::vsnprintf(buf, sizeof(buf), fmt, args);
    va_end(args);
    IhsTrampoline(level, tag, buf);
}

}  // namespace tsl
