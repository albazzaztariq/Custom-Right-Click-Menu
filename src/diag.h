// diag.h
// Tiny diagnostic logger. Writes to shell_extension.log next to the
// DLL so we can trace what happened during a hook test run.
//
// Disabled when NDEBUG is defined.

#pragma once

#include <string>

namespace diag {
    void log(const char* fmt, ...);
}
