#pragma once

#include "core/ihs.h"

namespace tsl {

// ihslib's log callback has no context pointer, so logging goes through one process-wide
// sink. The sink may be called from any thread.
using LogSink = void (*)(IHS_LogLevel level, const char *tag, const char *message);

void SetLogSink(LogSink sink);
void SetLogLevel(IHS_LogLevel maxLevel);

// Pass this to IHS_ClientSetLogFunction / IHS_SessionSetLogFunction.
IHS_LogFunction *IhsLogFunction();

void Log(IHS_LogLevel level, const char *tag, const char *fmt, ...) __attribute__((format(printf, 3, 4)));

}  // namespace tsl
