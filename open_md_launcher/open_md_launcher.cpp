// open_md_launcher.exe — Win11 .md double-click handler.
// Receives the source .md path as argv[1], copies it to the Temp
// Obsidian Vault (deduping the name on collision), opens it via the
// obsidian:// URL, then spawns obs_watcher.exe to clean up when the
// Obsidian window closes.
//
// Why this exists as an .exe and not a .bat: on Win11 24H2 Explorer's
// %1 substitution in HKCR\<progid>\shell\open\command does not reach
// .bat handlers in every shape we tried — the batch wound up seeing
// the literal string "%1" as argv[1]. For .exe targets the
// substitution is reliable.

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <shellapi.h>
#include <shlwapi.h>
#include <string>
#include <vector>
#include <cstdio>

#pragma comment(lib, "shlwapi.lib")
#pragma comment(lib, "shell32.lib")

static const wchar_t* kVaultPath = L"C:\\Users\\azt12\\Temp Obsidian Vault";
static const wchar_t* kVaultName = L"Temp Obsidian Vault";
static const wchar_t* kLogPath   =
    L"C:\\Users\\azt12\\open_md_launcher_debug.log";

static void Log(const wchar_t* fmt, ...) {
    FILE* f = nullptr;
    if (_wfopen_s(&f, kLogPath, L"ab") != 0 || !f) return;
    SYSTEMTIME st; ::GetLocalTime(&st);
    fwprintf(f, L"[%02d:%02d:%02d.%03d] ", st.wHour, st.wMinute,
             st.wSecond, st.wMilliseconds);
    va_list ap; va_start(ap, fmt);
    vfwprintf(f, fmt, ap);
    va_end(ap);
    fputwc(L'\n', f);
    fclose(f);
}

// Percent-encode a wide string per RFC 3986 unreserved set. Output is
// ASCII-safe (we encode non-ASCII as UTF-8 percent triplets).
static std::wstring UrlEncode(const std::wstring& s) {
    // Convert wide → UTF-8 first.
    int needed = ::WideCharToMultiByte(CP_UTF8, 0, s.c_str(),
                                       (int)s.size(), nullptr, 0,
                                       nullptr, nullptr);
    std::string utf8(needed, '\0');
    ::WideCharToMultiByte(CP_UTF8, 0, s.c_str(), (int)s.size(),
                          utf8.data(), needed, nullptr, nullptr);

    auto unreserved = [](unsigned char c) {
        return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
               (c >= '0' && c <= '9') ||
               c == '-' || c == '_' || c == '.' || c == '~';
    };
    std::wstring out;
    for (unsigned char c : utf8) {
        if (unreserved(c)) {
            out += (wchar_t)c;
        } else {
            wchar_t buf[5];
            swprintf_s(buf, 5, L"%%%02X", c);
            out += buf;
        }
    }
    return out;
}

static void EnsureDir(const std::wstring& p) {
    ::CreateDirectoryW(p.c_str(), nullptr);
}

static std::wstring DirOfThisExe() {
    wchar_t buf[MAX_PATH] = {};
    ::GetModuleFileNameW(nullptr, buf, MAX_PATH);
    std::wstring s = buf;
    size_t bs = s.find_last_of(L"\\/");
    if (bs != std::wstring::npos) s.resize(bs);
    return s;
}

int wmain(int argc, wchar_t** argv) {
    Log(L"=== open_md_launcher start (argc=%d) ===", argc);
    for (int i = 0; i < argc; ++i)
        Log(L"  argv[%d] = '%ls'", i, argv[i]);

    if (argc < 2) {
        Log(L"no file argument; exiting");
        return 1;
    }

    std::wstring src = argv[1];

    // Make sure the vault directory + .obsidian marker exist.
    EnsureDir(kVaultPath);
    EnsureDir(std::wstring(kVaultPath) + L"\\.obsidian");

    // Compute the leaf name (basename + ext).
    std::wstring leaf;
    {
        size_t bs = src.find_last_of(L"\\/");
        leaf = (bs == std::wstring::npos) ? src : src.substr(bs + 1);
    }
    std::wstring leaf_noext;
    {
        size_t dot = leaf.find_last_of(L'.');
        leaf_noext = (dot == std::wstring::npos) ? leaf
                                                 : leaf.substr(0, dot);
    }

    // Dedup against existing files in vault.
    std::wstring dst = std::wstring(kVaultPath) + L"\\" + leaf;
    int n = 2;
    while (::PathFileExistsW(dst.c_str())) {
        wchar_t nb[16];
        swprintf_s(nb, 16, L" (%d)", n++);
        dst = std::wstring(kVaultPath) + L"\\" + leaf_noext + nb + L".md";
    }

    Log(L"copying '%ls' → '%ls'", src.c_str(), dst.c_str());
    BOOL ok = ::CopyFileW(src.c_str(), dst.c_str(), TRUE);
    if (!ok) {
        DWORD err = ::GetLastError();
        Log(L"CopyFileW failed err=%lu", err);
        return 2;
    }

    // Recompute final leaf / leaf-no-ext from the actual destination.
    std::wstring final_leaf;
    std::wstring final_leaf_noext;
    {
        size_t bs = dst.find_last_of(L"\\/");
        final_leaf = (bs == std::wstring::npos) ? dst
                                                : dst.substr(bs + 1);
        size_t dot = final_leaf.find_last_of(L'.');
        final_leaf_noext = (dot == std::wstring::npos)
                              ? final_leaf
                              : final_leaf.substr(0, dot);
    }
    Log(L"final_leaf='%ls'", final_leaf.c_str());

    // Build the obsidian:// URL with both components percent-encoded.
    std::wstring url = L"obsidian://open?vault=" + UrlEncode(kVaultName) +
                       L"&file=" + UrlEncode(final_leaf);
    Log(L"url='%ls'", url.c_str());

    HINSTANCE rh = ::ShellExecuteW(nullptr, L"open", url.c_str(),
                                   nullptr, nullptr, SW_SHOWNORMAL);
    if ((INT_PTR)rh <= 32) {
        Log(L"ShellExecuteW failed rc=%lld", (long long)(INT_PTR)rh);
        // Don't return — still spawn the watcher so cleanup at least
        // runs if Obsidian eventually opens.
    } else {
        Log(L"ShellExecuteW ok");
    }

    // Spawn obs_watcher.exe sibling. Args:
    //   <file-to-delete> <leaf-no-ext> <vault-name>
    std::wstring watcher = DirOfThisExe() + L"\\obs_watcher.exe";
    if (!::PathFileExistsW(watcher.c_str())) {
        Log(L"obs_watcher.exe not found at '%ls'", watcher.c_str());
        return 0;
    }

    std::wstring cmdline = L"\"" + watcher + L"\" \"" + dst + L"\" \"" +
                           final_leaf_noext + L"\" \"" + kVaultName +
                           L"\"";

    STARTUPINFOW si = { sizeof(si) };
    PROCESS_INFORMATION pi = {};
    si.dwFlags = STARTF_USESHOWWINDOW;
    si.wShowWindow = SW_HIDE;
    // Detach via DETACHED_PROCESS so the watcher outlives us.
    BOOL cp = ::CreateProcessW(watcher.c_str(),
                               cmdline.data(),
                               nullptr, nullptr, FALSE,
                               DETACHED_PROCESS | CREATE_NO_WINDOW,
                               nullptr, nullptr, &si, &pi);
    if (!cp) {
        Log(L"CreateProcessW(watcher) failed err=%lu",
            ::GetLastError());
    } else {
        Log(L"watcher spawned pid=%lu", pi.dwProcessId);
        ::CloseHandle(pi.hProcess);
        ::CloseHandle(pi.hThread);
    }

    Log(L"=== open_md_launcher end ===");
    return 0;
}
