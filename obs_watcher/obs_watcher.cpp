// obs_watcher.cpp — event-driven cleanup for the "Open in Obsidian"
// flow. Sets system-wide SetWinEventHook listeners for
// EVENT_OBJECT_CREATE, EVENT_OBJECT_NAMECHANGE (to find the specific
// Obsidian window we care about) and EVENT_OBJECT_DESTROY (fired when
// that window closes).  When destroy fires for our window, deletes the
// temp file and exits.
//
//   obs_watcher.exe <file-to-delete> <filename-leaf-no-ext> <vault-name>
//
// All three args expected. <filename-leaf-no-ext> and <vault-name>
// are used to identify the right window by title — Obsidian's title
// is "<filename> - <vault name> - Obsidian", so a window whose title
// contains both uniquely identifies the one showing our temp file.
//
// No polling. The OS only calls our callback when window events
// actually happen.

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <string>
#include <cstdio>
#include <cstdarg>

static HWND        g_target = nullptr;
static std::wstring g_path;
static std::wstring g_leaf;
static std::wstring g_vault;

static void Log(const wchar_t* fmt, ...) {
    FILE* f = nullptr;
    wchar_t log_path[MAX_PATH];
    const wchar_t* tmp = _wgetenv(L"TEMP");
    if (!tmp) tmp = L".";
    _snwprintf_s(log_path, MAX_PATH, _TRUNCATE,
                 L"%s\\obs_watcher_debug.log", tmp);
    if (_wfopen_s(&f, log_path, L"ab") != 0 || !f) return;
    SYSTEMTIME st; ::GetLocalTime(&st);
    fwprintf(f, L"[%02d:%02d:%02d.%03d] ", st.wHour, st.wMinute,
             st.wSecond, st.wMilliseconds);
    va_list ap; va_start(ap, fmt);
    vfwprintf(f, fmt, ap);
    va_end(ap);
    fputwc(L'\n', f);
    fclose(f);
}

static bool TitleMatches(HWND h) {
    wchar_t buf[512] = {};
    int n = ::GetWindowTextW(h, buf, 512);
    if (n <= 0) return false;
    std::wstring t = buf;
    if (t.find(g_leaf) == std::wstring::npos) return false;
    if (!g_vault.empty() && t.find(g_vault) == std::wstring::npos)
        return false;
    return true;
}

static HWINEVENTHOOK g_hook_create_show = nullptr;
static HWINEVENTHOOK g_hook_namechange  = nullptr;

static void CALLBACK WinEventProc(HWINEVENTHOOK hHook, DWORD event,
                                  HWND hwnd, LONG idObject,
                                  LONG /*idChild*/, DWORD /*tid*/,
                                  DWORD /*time*/) {
    // Filter to top-level window events only (OBJID_WINDOW = 0).
    if (idObject != OBJID_WINDOW) return;
    if (!hwnd) return;

    // If we already have a target, only care about its destroy.
    if (g_target) {
        if (event == EVENT_OBJECT_DESTROY && hwnd == g_target) {
            wchar_t buf[512] = {};
            ::GetWindowTextW(hwnd, buf, 512);
            Log(L"DESTROY hwnd=%p title='%ls' target matched -> PostQuit",
                hwnd, buf);
            ::PostQuitMessage(0);
        }
        return;
    }

    // Target not yet found — listen for create, show, and name-change
    // events so we can identify the Obsidian window as soon as its
    // title is set (Obsidian is an Electron app; its XAML-island /
    // Chromium windows may not fire EVENT_OBJECT_SHOW reliably, but
    // EVENT_OBJECT_CREATE always fires and EVENT_OBJECT_NAMECHANGE
    // fires when the title is populated after the file loads).
    if ((event == EVENT_OBJECT_CREATE ||
         event == EVENT_OBJECT_SHOW) &&
        TitleMatches(hwnd)) {
        wchar_t buf[512] = {};
        ::GetWindowTextW(hwnd, buf, 512);
        Log(L"EVENT %lu captured hwnd=%p title='%ls'", event, hwnd, buf);
        g_target = hwnd;
        return;
    }
    if (event == EVENT_OBJECT_NAMECHANGE && TitleMatches(hwnd)) {
        wchar_t buf[512] = {};
        ::GetWindowTextW(hwnd, buf, 512);
        Log(L"NAMECHANGE captured hwnd=%p title='%ls'", hwnd, buf);
        g_target = hwnd;
        return;
    }
}

static BOOL CALLBACK EnumCatchupProc(HWND hwnd, LPARAM /*lp*/) {
    if (g_target) return FALSE;
    // Don't require IsWindowVisible — Obsidian may create the window
    // before it's visible, and the title may already be set.
    if (TitleMatches(hwnd)) {
        wchar_t buf[512] = {};
        ::GetWindowTextW(hwnd, buf, 512);
        Log(L"catch-up matched hwnd=%p title='%ls'", hwnd, buf);
        g_target = hwnd;
        return FALSE;
    }
    return TRUE;
}

int wmain(int argc, wchar_t** argv) {
    if (argc < 3) return 1;
    g_path  = argv[1];
    g_leaf  = argv[2];
    g_vault = (argc >= 4) ? argv[3] : L"";
    Log(L"=== obs_watcher start ===");
    Log(L"path='%ls'", g_path.c_str());
    Log(L"leaf='%ls'", g_leaf.c_str());
    Log(L"vault='%ls'", g_vault.c_str());

    // Two hooks:
    //  1. CREATE..SHOW — catches window creation and visibility changes.
    //  2. NAMECHANGE alone — catches title changes (Obsidian sets the
    //     title after loading the file, which may happen after the
    //     window is already visible).
    g_hook_create_show = ::SetWinEventHook(
        EVENT_OBJECT_CREATE, EVENT_OBJECT_SHOW,
        nullptr, WinEventProc,
        0, 0,
        WINEVENT_OUTOFCONTEXT | WINEVENT_SKIPOWNPROCESS);

    g_hook_namechange = ::SetWinEventHook(
        EVENT_OBJECT_NAMECHANGE, EVENT_OBJECT_NAMECHANGE,
        nullptr, WinEventProc,
        0, 0,
        WINEVENT_OUTOFCONTEXT | WINEVENT_SKIPOWNPROCESS);

    if (!g_hook_create_show && !g_hook_namechange) return 2;

    // Catch-up scan in case the Obsidian window appeared between
    // ShellExecute returning and us installing the hooks.
    ::EnumWindows(EnumCatchupProc, 0);
    Log(L"after catch-up scan, g_target=%p", g_target);

    // Standard Win32 message pump — blocks waiting for window events,
    // no busy loop.
    MSG msg;
    while (::GetMessageW(&msg, nullptr, 0, 0) > 0) {
        ::TranslateMessage(&msg);
        ::DispatchMessageW(&msg);
    }

    ::UnhookWinEvent(g_hook_create_show);
    ::UnhookWinEvent(g_hook_namechange);

    // Only delete if we ever saw the window — guards against the case
    // where Obsidian never opened (file association broken, vault
    // trust prompt declined, etc.). If g_target stayed null we'd
    // delete a file the user might still need.
    if (g_target) {
        BOOL ok = ::DeleteFileW(g_path.c_str());
        Log(L"DeleteFileW result=%d (err=%lu)", ok, ::GetLastError());
    } else {
        Log(L"exiting without delete — g_target never matched");
    }
    Log(L"=== obs_watcher end ===");
    return 0;
}
