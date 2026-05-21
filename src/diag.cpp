// diag.cpp
// File-based logger. Appends to shell_extension.log next to the DLL.

#include "diag.h"
#include <windows.h>
#include <cstdarg>
#include <cstdio>
#include <mutex>

namespace diag {

static std::mutex g_mutex;

static std::wstring log_path() {
    HMODULE self = nullptr;
    ::GetModuleHandleExW(
        GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
        GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
        reinterpret_cast<LPCWSTR>(&log_path), &self);

    wchar_t buf[MAX_PATH] = {};
    ::GetModuleFileNameW(self, buf, MAX_PATH);
    std::wstring p(buf);
    auto slash = p.find_last_of(L"\\/");
    if (slash != std::wstring::npos) p.resize(slash + 1);
    p += L"shell_extension.log";
    return p;
}

void log(const char* fmt, ...) {
    std::lock_guard<std::mutex> lock(g_mutex);
    FILE* f = nullptr;
    _wfopen_s(&f, log_path().c_str(), L"a");
    if (!f) return;

    SYSTEMTIME st{};
    ::GetLocalTime(&st);
    fprintf(f, "[%02d:%02d:%02d.%03d] ",
            st.wHour, st.wMinute, st.wSecond, st.wMilliseconds);

    va_list args;
    va_start(args, fmt);
    vfprintf(f, fmt, args);
    va_end(args);

    fprintf(f, "\n");
    fclose(f);
}

}  // namespace diag
