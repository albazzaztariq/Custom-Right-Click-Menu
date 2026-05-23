// menu.cpp
// Consumer-defined right-click menu. Every entry in RegisterMenu uses
// only the Selection / Submenu / Separator macros from menu_api.h.
// Action implementations live in helper functions above so the menu
// body itself stays one line per item.

#include "../src/menu_api.h"

#include <windows.h>
#include <shellapi.h>
#include <shlobj.h>
#include <shobjidl.h>
#include <exdisp.h>
#include <objbase.h>
#include <oleacc.h>
#include <string>
#include <vector>
#include <algorithm>

// ─────────────────────────────────────────────────────────────────────
// Helpers
// ─────────────────────────────────────────────────────────────────────

static std::wstring widen(const std::string& s) {
    if (s.empty()) return {};
    int n = ::MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(),
                                  nullptr, 0);
    std::wstring out(n, L'\0');
    ::MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(),
                          out.data(), n);
    return out;
}

// Synthesize a keypress (optionally with Ctrl and/or Shift held).
// Used for actions whose shell verb is unreliable or where mimicking
// the keyboard shortcut is simplest (Copy / Cut / Delete / Rename /
// New-folder / Refresh).
static void SendKeys(WORD vk, bool ctrl = false, bool shift = false) {
    INPUT in[6] = {};
    int n = 0;
    if (ctrl)  { in[n].type = INPUT_KEYBOARD; in[n].ki.wVk = VK_CONTROL; ++n; }
    if (shift) { in[n].type = INPUT_KEYBOARD; in[n].ki.wVk = VK_SHIFT;   ++n; }
    in[n].type = INPUT_KEYBOARD; in[n].ki.wVk = vk; ++n;
    in[n].type = INPUT_KEYBOARD; in[n].ki.wVk = vk;
    in[n].ki.dwFlags = KEYEVENTF_KEYUP; ++n;
    if (shift) { in[n].type = INPUT_KEYBOARD; in[n].ki.wVk = VK_SHIFT;
                 in[n].ki.dwFlags = KEYEVENTF_KEYUP; ++n; }
    if (ctrl)  { in[n].type = INPUT_KEYBOARD; in[n].ki.wVk = VK_CONTROL;
                 in[n].ki.dwFlags = KEYEVENTF_KEYUP; ++n; }
    ::SendInput(n, in, sizeof(INPUT));
}

// Alt+Enter chord for Properties.
static void SendAltEnter() {
    INPUT in[4] = {};
    in[0].type = INPUT_KEYBOARD; in[0].ki.wVk = VK_MENU;
    in[1].type = INPUT_KEYBOARD; in[1].ki.wVk = VK_RETURN;
    in[2].type = INPUT_KEYBOARD; in[2].ki.wVk = VK_RETURN;
    in[2].ki.dwFlags = KEYEVENTF_KEYUP;
    in[3].type = INPUT_KEYBOARD; in[3].ki.wVk = VK_MENU;
    in[3].ki.dwFlags = KEYEVENTF_KEYUP;
    ::SendInput(4, in, sizeof(INPUT));
}

// Push UTF-8 text onto the clipboard as CF_UNICODETEXT (so it round-
// trips through explorer's address bar, which is wide-string).
static void CopyTextToClipboard(const std::string& text) {
    std::wstring w = widen(text);
    if (!::OpenClipboard(nullptr)) return;
    ::EmptyClipboard();
    size_t bytes = (w.size() + 1) * sizeof(wchar_t);
    HGLOBAL h = ::GlobalAlloc(GMEM_MOVEABLE, bytes);
    if (h) {
        void* dst = ::GlobalLock(h);
        memcpy(dst, w.c_str(), bytes);
        ::GlobalUnlock(h);
        ::SetClipboardData(CF_UNICODETEXT, h);
    }
    ::CloseClipboard();
}

// Canonical "Copy as Path" — what Explorer's own Ctrl+C does.
// SHCreateItemFromParsingName(path) -> IShellItem ->
// BindToHandler(BHID_DataObject) -> IDataObject -> OleSetClipboard,
// then OleFlushClipboard so the data persists after we release the
// data object. The shell-built IDataObject carries CF_HDROP, the
// Shell IDList Array, and CF_UNICODETEXT (the path text), so every
// reasonable paste target — Explorer's address bar, Notepad, etc. —
// gets what it expects. Bypasses ShellExecuteExA verb-lookup (which
// can't reach "copyaspath" on Win11) and raw OpenClipboard
// (which orphans the data inside explorer.exe).
static bool CopyAsPathViaShell(const std::string& p) {
    HRESULT ohr = ::OleInitialize(nullptr);
    bool need_oleuninit = (ohr == S_OK);
    bool ok = false;

    std::wstring wp = widen(p);
    IShellItem* item = nullptr;
    if (SUCCEEDED(::SHCreateItemFromParsingName(
            wp.c_str(), nullptr, IID_PPV_ARGS(&item))) && item) {
        IDataObject* dobj = nullptr;
        if (SUCCEEDED(item->BindToHandler(
                nullptr, BHID_DataObject,
                IID_PPV_ARGS(&dobj))) && dobj) {
            HRESULT shr = ::OleSetClipboard(dobj);
            ok = SUCCEEDED(shr);
            // Render the data into static clipboard formats so paste
            // still works after we release our IDataObject reference.
            ::OleFlushClipboard();
            dobj->Release();
        }
        item->Release();
    }

    if (need_oleuninit) ::OleUninitialize();
    return ok;
}

// Force the cabinet (Explorer file-list frame) to be foreground and
// focused. Our menu's dismissal-via-selection path leaves a tiny
// focus-transition window during which synthesized keys get dropped
// (system bell). Routing input here first eliminates that.
//
// Win11 quirk: SetForegroundWindow silently fails if our thread is
// not the last-input-thread (returns FALSE). AllowSetForegroundWindow
// + AttachThreadInput unlocks it.
static HWND FocusCabinet() {
    HWND cab = ::FindWindowW(L"CabinetWClass", nullptr);
    if (!cab) return nullptr;
    DWORD cab_tid = ::GetWindowThreadProcessId(cab, nullptr);
    DWORD our_tid = ::GetCurrentThreadId();
    ::AllowSetForegroundWindow(ASFW_ANY);
    bool attached = false;
    if (cab_tid != our_tid) {
        attached = ::AttachThreadInput(our_tid, cab_tid, TRUE) != 0;
    }
    ::BringWindowToTop(cab);
    ::SetForegroundWindow(cab);
    ::SetFocus(cab);
    // Pump posted activation/focus messages until the queue is empty
    // OR 80ms elapses. PeekMessage returning FALSE just means the
    // queue is momentarily empty — wait briefly and retry.
    MSG m;
    DWORD t0 = ::GetTickCount();
    while (::GetTickCount() - t0 < 80) {
        if (::PeekMessageW(&m, nullptr, 0, 0, PM_REMOVE)) {
            ::TranslateMessage(&m);
            ::DispatchMessageW(&m);
        } else {
            Sleep(5);
        }
    }
    if (attached) ::AttachThreadInput(our_tid, cab_tid, FALSE);
    return cab;
}

// Find the keyboard-focus descendant of the cabinet — typically a
// ShellTabWindowClass / DirectUIHWND / NetUIHWND / SysListView32
// child. This is the window Ctrl+T (and other accelerators) must
// be routed to. Returns the cabinet itself as fallback.
static HWND FocusedDescendant(HWND cab) {
    if (!cab) return nullptr;
    DWORD cab_tid = ::GetWindowThreadProcessId(cab, nullptr);
    DWORD our_tid = ::GetCurrentThreadId();
    HWND focused = nullptr;
    bool attached = false;
    if (cab_tid != our_tid) {
        attached = ::AttachThreadInput(our_tid, cab_tid, TRUE) != 0;
    }
    focused = ::GetFocus();
    if (attached) ::AttachThreadInput(our_tid, cab_tid, FALSE);
    return focused ? focused : cab;
}

// Find the Microsoft.UI.Content.DesktopChildSiteBridge child of the
// cabinet. On Win11 24H2 the tab strip and its Ctrl+T accelerator are
// owned by this WinUI3/Xaml-Islands bridge child, NOT by the legacy
// CabinetWClass frame. SendInput Ctrl+T directed at the frame is
// silently absorbed (the bridge consumes input first); routing focus
// to the bridge itself before SendInput is what makes it land. This is
// the approach used by the open-source ExplorerTabUtility project.
struct BridgeCtx { HWND result; };
static BOOL CALLBACK find_bridge_proc(HWND hwnd, LPARAM lp) {
    auto* ctx = reinterpret_cast<BridgeCtx*>(lp);
    wchar_t cls[160] = {};
    ::GetClassNameW(hwnd, cls, 160);
    if (wcsstr(cls, L"DesktopChildSiteBridge") != nullptr) {
        ctx->result = hwnd;
        return FALSE;       // stop on first match
    }
    return TRUE;
}
static HWND FindBridge(HWND cab) {
    if (!cab) return nullptr;
    BridgeCtx ctx{ nullptr };
    ::EnumChildWindows(cab, find_bridge_proc, (LPARAM)&ctx);
    return ctx.result;
}

// Move focus to the WinUI bridge so the next SendInput chord routes
// through the right accelerator owner. AttachThreadInput is needed
// because the bridge runs on its own input queue in some builds.
static void FocusBridge(HWND cab) {
    HWND bridge = FindBridge(cab);
    if (!bridge) return;
    DWORD tid = ::GetWindowThreadProcessId(bridge, nullptr);
    DWORD our = ::GetCurrentThreadId();
    bool attached = (tid != our)
        ? ::AttachThreadInput(our, tid, TRUE) != 0
        : false;
    ::SetFocus(bridge);
    if (attached) ::AttachThreadInput(our, tid, FALSE);
    Sleep(30);
}

// Post a Ctrl+chord keystroke to a specific HWND, bypassing the
// foreground-input system entirely. PostMessage delivers WM_KEYDOWN/
// WM_KEYUP straight to the window's message queue; the receiving
// window's accelerator logic checks GetKeyState() for modifiers,
// which we satisfy by holding Ctrl down on the global input state
// via a brief SendInput chord around the post. This is what makes
// the receiving window's IsDialogMessage / TranslateAccelerator see
// Ctrl as pressed when it inspects key state.
static void PostCtrlKey(HWND target, WORD vk) {
    // Press Ctrl globally (so GetKeyState in the target sees it).
    INPUT down = {}; down.type = INPUT_KEYBOARD;
    down.ki.wVk = VK_CONTROL;
    ::SendInput(1, &down, sizeof(INPUT));

    // Post the actual key to the target. lParam high bits encode
    // scancode/extended/context — minimal values suffice for most
    // accelerator handlers.
    UINT scan = ::MapVirtualKeyW(vk, MAPVK_VK_TO_VSC);
    LPARAM lpDown = (1) | (scan << 16);
    LPARAM lpUp   = (1) | (scan << 16) | (1u << 30) | (1u << 31);
    ::PostMessageW(target, WM_KEYDOWN, vk, lpDown);
    ::PostMessageW(target, WM_KEYUP,   vk, lpUp);

    Sleep(20);

    // Release Ctrl globally.
    INPUT up = {}; up.type = INPUT_KEYBOARD;
    up.ki.wVk = VK_CONTROL;
    up.ki.dwFlags = KEYEVENTF_KEYUP;
    ::SendInput(1, &up, sizeof(INPUT));
}

// Navigate the current explorer tab to `p`. With FocusCabinet now
// using AttachThreadInput + AllowSetForegroundWindow + 80ms pump,
// SendInput-based keystrokes should land on the proper accelerator
// scope without the bell.
static void NavigateHere(const std::string& p) {
    FocusCabinet();
    CopyTextToClipboard(p);
    SendKeys('L', /*ctrl=*/true);
    Sleep(120);
    SendKeys('V', /*ctrl=*/true);
    Sleep(60);
    SendKeys(VK_RETURN);
}

// Debug log for the new-tab pipeline. Writes to a file we read after
// each test to see exactly what classes / HWNDs / HRESULTs are
// produced on the live 24H2 build 26200.
static void nt_log(const char* fmt, ...) {
    FILE* f = nullptr;
    if (fopen_s(&f, "C:\\Users\\azt12\\rcm_newtab_debug.log", "ab") != 0
        || !f) return;
    SYSTEMTIME st; ::GetLocalTime(&st);
    fprintf(f, "[%02d:%02d:%02d.%03d] ", st.wHour, st.wMinute,
            st.wSecond, st.wMilliseconds);
    va_list ap; va_start(ap, fmt);
    vfprintf(f, fmt, ap);
    va_end(ap);
    fputc('\n', f);
    fclose(f);
}

// Snapshot HWNDs of all ShellTabWindowClass children of the cabinet.
// Used to detect which tab is freshly created after WM_COMMAND 0xA21B.
// Also dumps EVERY child class+hwnd to the debug log so we can see what
// Win11 24H2 actually exposes inside CabinetWClass on this build.
struct TabEnumCtx { std::vector<HWND>* tabs; int dump; };
static BOOL CALLBACK enum_tabs_proc(HWND hw, LPARAM lp) {
    auto* ctx = reinterpret_cast<TabEnumCtx*>(lp);
    wchar_t cls[128] = {};
    ::GetClassNameW(hw, cls, 128);
    if (ctx->dump) nt_log("  child hwnd=%p class=%ls", hw, cls);
    if (wcscmp(cls, L"ShellTabWindowClass") == 0) {
        ctx->tabs->push_back(hw);
    }
    return TRUE;
}
static void SnapshotTabs(HWND cab, std::vector<HWND>& out, int dump = 0) {
    out.clear();
    TabEnumCtx ctx{ &out, dump };
    ::EnumChildWindows(cab, enum_tabs_proc, (LPARAM)&ctx);
}

// Poll up to `timeout_ms` for a NEW ShellTabWindowClass child to appear
// under `cab` that wasn't in `before`. Returns the new HWND or nullptr.
static HWND WaitForNewTab(HWND cab, const std::vector<HWND>& before,
                          DWORD timeout_ms) {
    DWORD start = ::GetTickCount();
    while (::GetTickCount() - start < timeout_ms) {
        std::vector<HWND> now;
        SnapshotTabs(cab, now);
        for (HWND h : now) {
            if (std::find(before.begin(), before.end(), h) ==
                before.end()) {
                return h;
            }
        }
        Sleep(50);
    }
    return nullptr;
}

// Walk IShellWindows looking for the entry whose IWebBrowser2::HWND
// matches `target_hwnd`. On Win11 24H2 (build 26200), IShellWindows
// has ONE entry per CabinetWClass and IWebBrowser2::get_HWND returns
// the cabinet HWND — there is no per-tab entry. So callers should
// pass the CABINET HWND, not a ShellTabWindowClass HWND. After
// WM_COMMAND 0xA21B opens a new tab, that tab is the active one in
// the cabinet, so Navigate2 on this single browser navigates it.
// Caller releases the returned pointer.
static IWebBrowser2* FindBrowserByHwnd(HWND target_hwnd, DWORD timeout_ms) {
    DWORD start = ::GetTickCount();
    while (::GetTickCount() - start < timeout_ms) {
        IShellWindows* psw = nullptr;
        if (SUCCEEDED(::CoCreateInstance(CLSID_ShellWindows, nullptr,
                                         CLSCTX_ALL, IID_PPV_ARGS(&psw)))
            && psw) {
            long count = 0;
            psw->get_Count(&count);
            for (long i = 0; i < count; ++i) {
                VARIANT vi; ::VariantInit(&vi);
                vi.vt = VT_I4; vi.lVal = i;
                IDispatch* disp = nullptr;
                if (FAILED(psw->Item(vi, &disp)) || !disp) continue;

                IWebBrowser2* wb = nullptr;
                if (SUCCEEDED(disp->QueryInterface(
                        IID_PPV_ARGS(&wb))) && wb) {
                    SHANDLE_PTR hp = 0;
                    if (SUCCEEDED(wb->get_HWND(&hp))
                        && reinterpret_cast<HWND>(hp) == target_hwnd) {
                        disp->Release();
                        psw->Release();
                        return wb;
                    }
                    wb->Release();
                }
                disp->Release();
            }
            psw->Release();
        }
        Sleep(50);
    }
    return nullptr;
}

// Find first descendant of `parent` whose class name matches `cls`.
struct FindChildCtx { const wchar_t* cls; HWND result; };
static BOOL CALLBACK find_child_proc(HWND hw, LPARAM lp) {
    auto* ctx = reinterpret_cast<FindChildCtx*>(lp);
    wchar_t cn[128] = {};
    ::GetClassNameW(hw, cn, 128);
    if (wcscmp(cn, ctx->cls) == 0) { ctx->result = hw; return FALSE; }
    return TRUE;
}
static HWND FindChildByClass(HWND parent, const wchar_t* cls) {
    FindChildCtx ctx{ cls, nullptr };
    ::EnumChildWindows(parent, find_child_proc, (LPARAM)&ctx);
    return ctx.result;
}

// Obtain the per-tab IShellBrowser via the "native object model"
// accessibility backdoor. On Win11 24H2 tabbed Explorer, IShellWindows
// doesn't enumerate per-tab, so this is the only way to reach a
// SPECIFIC tab's IShellBrowser. Try the tab HWND first, then a few
// well-known descendant classes. Caller releases.
static IShellBrowser* GetTabShellBrowser(HWND tab_hwnd) {
    HWND candidates[5] = {
        tab_hwnd,
        FindChildByClass(tab_hwnd, L"SHELLDLL_DefView"),
        FindChildByClass(tab_hwnd, L"FileExplorer_FolderViewIsland"),
        FindChildByClass(tab_hwnd,
            L"Microsoft.UI.Content.DesktopChildSiteBridge"),
        FindChildByClass(tab_hwnd, L"DirectUIHWND"),
    };
    for (HWND h : candidates) {
        if (!h) continue;
        LRESULT lr = ::SendMessageW(h, WM_GETOBJECT, 0, OBJID_NATIVEOM);
        if (lr == 0) {
            nt_log("  WM_GETOBJECT hwnd=%p lr=0", h);
            continue;
        }
        IDispatch* disp = nullptr;
        HRESULT hr = ::ObjectFromLresult(lr, IID_IDispatch, 0,
                                         (void**)&disp);
        nt_log("  WM_GETOBJECT hwnd=%p ObjectFromLresult hr=0x%08x "
               "disp=%p", h, hr, (void*)disp);
        if (FAILED(hr) || !disp) continue;
        IServiceProvider* sp = nullptr;
        if (SUCCEEDED(disp->QueryInterface(IID_PPV_ARGS(&sp))) && sp) {
            IShellBrowser* sb = nullptr;
            if (SUCCEEDED(sp->QueryService(SID_SShellBrowser,
                                           IID_PPV_ARGS(&sb))) && sb) {
                nt_log("  got per-tab IShellBrowser from hwnd=%p", h);
                sp->Release();
                disp->Release();
                return sb;
            }
            sp->Release();
        }
        disp->Release();
    }
    return nullptr;
}

// New-tab variant (ExplorerTabUtility pattern). Sequence:
//   1) Snapshot existing ShellTabWindowClass children of CabinetWClass.
//   2) Post WM_COMMAND 0xA21B to create a new tab.
//   3) Poll up to 2 s for a NEW ShellTabWindowClass to appear.
//   4) Poll IShellWindows for the entry whose IWebBrowser2::HWND
//      matches the new tab HWND (registration is async).
//   5) Call IWebBrowser2::Navigate2(path) on that specific browser.
//
// Why this and not IShellBrowser::BrowseObject: on Win11 24H2 tabbed
// Explorer, SID_STopLevelBrowser returns whichever tab is currently
// active, which post-WM_COMMAND can be the OLD tab (the new tab is
// created asynchronously). BrowseObject then navigates the wrong
// tab and the new one stays on Home. Navigate2 on the per-tab
// IWebBrowser2 we matched by HWND is what ExplorerTabUtility uses.
static void OpenInNewTab(const std::string& p) {
    nt_log("== OpenInNewTab path=%s ==", p.c_str());
    HWND cab = ::FindWindowW(L"CabinetWClass", nullptr);
    nt_log("CabinetWClass hwnd=%p", cab);
    if (!cab) return;

    HRESULT chr = ::CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    bool need_co = (chr == S_OK);

    // Dump every child window class+HWND under the cabinet so we can
    // see what class names Win11 24H2 26200 actually uses (the
    // ExplorerTabUtility-cited "ShellTabWindowClass" may be obsolete).
    std::vector<HWND> before;
    nt_log("--- BEFORE WM_COMMAND, cabinet children ---");
    SnapshotTabs(cab, before, /*dump=*/1);
    nt_log("ShellTabWindowClass count (before) = %zu", before.size());

    // Also dump all IShellWindows entries pre-command for comparison.
    {
        IShellWindows* psw = nullptr;
        if (SUCCEEDED(::CoCreateInstance(CLSID_ShellWindows, nullptr,
                                         CLSCTX_ALL, IID_PPV_ARGS(&psw)))
            && psw) {
            long c = 0; psw->get_Count(&c);
            nt_log("IShellWindows count (before) = %ld", c);
            for (long i = 0; i < c; ++i) {
                VARIANT vi; ::VariantInit(&vi);
                vi.vt = VT_I4; vi.lVal = i;
                IDispatch* d = nullptr;
                if (SUCCEEDED(psw->Item(vi, &d)) && d) {
                    IWebBrowser2* wb = nullptr;
                    if (SUCCEEDED(d->QueryInterface(IID_PPV_ARGS(&wb)))
                        && wb) {
                        SHANDLE_PTR hp = 0; wb->get_HWND(&hp);
                        nt_log("  isw[%ld] hwnd=%p", i, (HWND)hp);
                        wb->Release();
                    }
                    d->Release();
                }
            }
            psw->Release();
        }
    }

    nt_log("Sending WM_COMMAND 0xA21B...");
    LRESULT sr = ::SendMessageW(cab, WM_COMMAND, 0xA21B, 0);
    nt_log("WM_COMMAND returned %lld", (long long)sr);

    nt_log("--- AFTER WM_COMMAND, cabinet children ---");
    std::vector<HWND> after;
    SnapshotTabs(cab, after, /*dump=*/1);
    nt_log("ShellTabWindowClass count (after) = %zu", after.size());

    HWND new_tab = WaitForNewTab(cab, before, 2000);
    nt_log("WaitForNewTab result = %p", new_tab);
    if (new_tab) {
        Sleep(200);  // let new tab register in IShellWindows

        // Diagnostic: dump IShellWindows count and each entry's HWND
        // + LocationURL AFTER WM_COMMAND. If count went 1->2, both
        // entries share cabinet HWND and we need to pick the new one.
        // The new tab is typically appended LAST in the collection.
        IWebBrowser2* target = nullptr;
        IShellWindows* psw = nullptr;
        if (SUCCEEDED(::CoCreateInstance(CLSID_ShellWindows, nullptr,
                                         CLSCTX_ALL, IID_PPV_ARGS(&psw)))
            && psw) {
            long count = 0;
            psw->get_Count(&count);
            nt_log("IShellWindows count (after) = %ld", count);
            for (long i = 0; i < count; ++i) {
                VARIANT vi; ::VariantInit(&vi);
                vi.vt = VT_I4; vi.lVal = i;
                IDispatch* disp = nullptr;
                if (FAILED(psw->Item(vi, &disp)) || !disp) continue;
                IWebBrowser2* wb = nullptr;
                if (SUCCEEDED(disp->QueryInterface(IID_PPV_ARGS(&wb)))
                    && wb) {
                    SHANDLE_PTR hp = 0;
                    wb->get_HWND(&hp);
                    BSTR url = nullptr;
                    wb->get_LocationURL(&url);
                    nt_log("  isw[%ld] hwnd=%p url=%ls",
                           i, (HWND)hp, url ? url : L"<null>");
                    if (url) ::SysFreeString(url);
                    if ((HWND)hp == cab) {
                        // Keep replacing — final value will be the
                        // LAST entry matching the cabinet, which on
                        // tabbed Explorer is the newest tab's browser.
                        if (target) target->Release();
                        target = wb;
                        wb = nullptr;
                    }
                }
                if (wb) wb->Release();
                disp->Release();
            }
            psw->Release();
        }
        nt_log("selected target IWebBrowser2 = %p (LAST cabinet match)",
               (void*)target);

        if (target) {
            std::wstring wp = widen(p);
            VARIANT url; ::VariantInit(&url);
            url.vt = VT_BSTR;
            url.bstrVal = ::SysAllocString(wp.c_str());
            HRESULT nhr = target->Navigate2(&url, nullptr, nullptr,
                                            nullptr, nullptr);
            nt_log("Navigate2 hr=0x%08x", nhr);
            ::VariantClear(&url);
            target->Release();
        }
    }

    if (need_co) ::CoUninitialize();
    nt_log("== done ==");
}

// Spawn a brand-new explorer.exe rooted at `path`.
static void OpenInNewWindow(const std::string& p) {
    std::string arg = "\"" + p + "\"";
    ::ShellExecuteA(nullptr, "open", "explorer.exe", arg.c_str(),
                    nullptr, SW_SHOWNORMAL);
}

// Launch Windows Terminal as ADMIN with its starting directory set
// to `folder`. Two subtleties on this box:
//   (1) wt's -d sets the initial working dir, but a PowerShell
//       profile that Set-Locations elsewhere (e.g. to Computing on
//       this machine) runs AFTER the wt -d, overriding it. We add
//       an inline Set-Location -LiteralPath '<folder>' via
//       -NoExit -Command so the profile loads first, then we cd
//       back to where the user actually right-clicked.
//   (2) ShellExecute with verb "runas" triggers the UAC prompt and
//       launches the process elevated.
// wt.exe is on PATH on Win11 by default.
static void OpenTerminalAtAsAdmin(const std::string& folder) {
    if (folder.empty()) return;
    std::string args =
        "-d \"" + folder + "\" "
        "powershell -NoExit -Command \"Set-Location -LiteralPath '" +
        folder + "'\"";

    SHELLEXECUTEINFOA sei = { sizeof(sei) };
    sei.fMask        = 0;
    sei.lpVerb       = "runas";
    sei.lpFile       = "wt.exe";
    sei.lpParameters = args.c_str();
    sei.nShow        = SW_SHOWNORMAL;
    ::ShellExecuteExA(&sei);
}

// Create an empty "New Text Document.txt" in `folder`. If a file by
// that name already exists, suffix with " (2)", " (3)" etc. until
// unique. Notifies the shell so Explorer's view refreshes and shows
// the new file.
static void CreateNewTextFile(const std::string& folder) {
    if (folder.empty()) return;
    std::wstring wfolder = widen(folder);
    std::wstring base    = L"New Text Document";
    std::wstring ext     = L".txt";
    std::wstring name    = base + ext;
    std::wstring full    = wfolder + L"\\" + name;
    for (int n = 2; n < 1000 &&
         ::GetFileAttributesW(full.c_str()) != INVALID_FILE_ATTRIBUTES;
         ++n) {
        name = base + L" (" + std::to_wstring(n) + L")" + ext;
        full = wfolder + L"\\" + name;
    }
    HANDLE h = ::CreateFileW(full.c_str(), GENERIC_WRITE, 0, nullptr,
                             CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h != INVALID_HANDLE_VALUE) {
        ::CloseHandle(h);
        ::SHChangeNotify(SHCNE_CREATE, SHCNF_PATHW, full.c_str(), nullptr);
    }
}

// Invoke a shell verb on a filesystem path via ShellExecuteEx.
// Returns true on success. NO_UI suppresses "what to open with" UI.
static bool InvokeVerb(const char* verb, const std::string& p) {
    SHELLEXECUTEINFOA sei = { sizeof(sei) };
    sei.fMask  = SEE_MASK_FLAG_NO_UI;
    sei.lpVerb = verb;
    sei.lpFile = p.c_str();
    sei.nShow  = SW_SHOWNORMAL;
    return ::ShellExecuteExA(&sei) == TRUE;
}

// Create a .lnk shortcut on the user's Desktop pointing at `target`.
static void CreateDesktopShortcut(const std::string& target) {
    HRESULT hr = ::CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    bool need_couninit = (hr == S_OK);

    IShellLinkW* link = nullptr;
    if (SUCCEEDED(::CoCreateInstance(CLSID_ShellLink, nullptr,
                                     CLSCTX_INPROC_SERVER,
                                     IID_PPV_ARGS(&link))) && link) {
        std::wstring wtarget = widen(target);
        link->SetPath(wtarget.c_str());

        IPersistFile* pf = nullptr;
        if (SUCCEEDED(link->QueryInterface(IID_PPV_ARGS(&pf))) && pf) {
            wchar_t desktop[MAX_PATH] = {};
            if (SUCCEEDED(::SHGetFolderPathW(nullptr, CSIDL_DESKTOPDIRECTORY,
                                             nullptr, 0, desktop))) {
                // Derive the .lnk name from the folder leaf.
                size_t slash = target.find_last_of("\\/");
                std::string leaf = (slash == std::string::npos)
                                 ? target : target.substr(slash + 1);
                std::wstring wleaf = widen(leaf);
                std::wstring lnk = std::wstring(desktop) + L"\\"
                                 + wleaf + L".lnk";
                pf->Save(lnk.c_str(), TRUE);
            }
            pf->Release();
        }
        link->Release();
    }

    if (need_couninit) ::CoUninitialize();
}

// Compress a folder to <folder>.zip next to it via Windows PowerShell
// 5.1 Compress-Archive. Hardening over the previous attempt:
//   - -ExecutionPolicy Bypass (24H2 Insider often defaults to
//     AllSigned which silently blocks the implicit module load);
//   - Full path to powershell.exe (don't rely on PATH);
//   - Wait on the process and read the exit code (silent failures
//     previously left no trace);
//   - PowerShell -EncodedCommand to sidestep CMD's quoting hell with
//     paths containing spaces, ampersands, single-quotes, etc.
// Run an arbitrary PowerShell command string via -EncodedCommand
// (Base64 UTF-16LE). Bypasses CMD's metacharacter parsing so paths
// with spaces, ampersands, single quotes etc. don't break the
// invocation. If detached=false (default), waits up to 30 s for
// completion. If detached=true, returns immediately and the PS
// process lives independently — used for background watchers like
// the Obsidian temp-vault cleanup.
static void RunPowerShellEncoded(const std::wstring& ps1,
                                 bool detached = false) {
    auto base64 = [](const BYTE* data, size_t n) {
        static const char* tab =
            "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
        std::string out;
        for (size_t i = 0; i < n; i += 3) {
            uint32_t v = data[i] << 16;
            if (i + 1 < n) v |= data[i + 1] << 8;
            if (i + 2 < n) v |= data[i + 2];
            out += tab[(v >> 18) & 63];
            out += tab[(v >> 12) & 63];
            out += (i + 1 < n) ? tab[(v >> 6) & 63] : '=';
            out += (i + 2 < n) ? tab[v & 63] : '=';
        }
        return out;
    };
    std::string enc = base64(
        reinterpret_cast<const BYTE*>(ps1.data()),
        ps1.size() * sizeof(wchar_t));

    std::string cmd =
        "C:\\Windows\\System32\\WindowsPowerShell\\v1.0\\powershell.exe "
        "-NoProfile -ExecutionPolicy Bypass -WindowStyle Hidden "
        "-EncodedCommand " + enc;

    STARTUPINFOA si = { sizeof(si) };
    si.dwFlags     = STARTF_USESHOWWINDOW;
    si.wShowWindow = SW_HIDE;
    PROCESS_INFORMATION pi = {};
    DWORD flags = CREATE_NO_WINDOW;
    if (detached) flags |= DETACHED_PROCESS | CREATE_BREAKAWAY_FROM_JOB;
    if (!::CreateProcessA(nullptr, (LPSTR)cmd.c_str(), nullptr, nullptr,
                          FALSE, flags, nullptr, nullptr, &si, &pi)) return;
    if (!detached) ::WaitForSingleObject(pi.hProcess, 30000);
    ::CloseHandle(pi.hProcess);
    ::CloseHandle(pi.hThread);
}

// Compress a single folder or file into `<original>.zip` next to it.
static void CompressToZip(const std::string& p) {
    std::string zip = p + ".zip";
    std::wstring ps1 =
        L"Compress-Archive -LiteralPath '" + widen(p) +
        L"' -DestinationPath '" + widen(zip) + L"' -Force";
    RunPowerShellEncoded(ps1);
}

// Compress a multi-selection into a SINGLE archive in the parent
// folder of the first item. The archive is named `Archive.zip`, with
// a `(2)`/`(3)` suffix if that name already exists. Used by the
// MultiSelection right-click menu.
static void CompressMultipleToZip(const std::vector<std::string>& items) {
    if (items.empty()) return;
    size_t slash = items[0].find_last_of("\\/");
    if (slash == std::string::npos) return;
    std::string parent = items[0].substr(0, slash);

    std::wstring wparent = widen(parent);
    std::wstring full    = wparent + L"\\Archive.zip";
    for (int n = 2; n < 1000 &&
         ::GetFileAttributesW(full.c_str()) != INVALID_FILE_ATTRIBUTES;
         ++n) {
        full = wparent + L"\\Archive (" + std::to_wstring(n) + L").zip";
    }

    // Compress-Archive -LiteralPath @('p1','p2',...) -DestinationPath '<z>'
    std::wstring ps1 = L"Compress-Archive -LiteralPath @(";
    for (size_t i = 0; i < items.size(); ++i) {
        if (i > 0) ps1 += L",";
        ps1 += L"'" + widen(items[i]) + L"'";
    }
    ps1 += L") -DestinationPath '" + full + L"' -Force";
    RunPowerShellEncoded(ps1);
}


// ─────────────────────────────────────────────────────────────────────
// .md / Obsidian flow
//
// We don't want to be locked into Obsidian's "Vault" model just to
// VIEW an .md file. So:
//   1. Copy the .md to C:\Users\azt12\Temp Obsidian Vault (creating
//      it as a minimal vault on first use by writing a .obsidian
//      marker folder so Obsidian recognizes it).
//   2. Open via the obsidian://open?vault=...&file=... URL scheme
//      so Obsidian opens THAT specific vault — opens a new window
//      if Obsidian is already running for the user's main vault.
//   3. Spawn a detached PowerShell that polls every 2 s for the
//      Obsidian process to exit, then deletes the temp copy.
//   4. Register a RunOnce HKCU entry as a fallback cleanup — if the
//      watcher dies on shutdown/crash, the file gets cleaned up on
//      next logon.
// ─────────────────────────────────────────────────────────────────────

static const char* kTempVaultPath = "C:\\Users\\azt12\\Temp Obsidian Vault";
static const char* kMainVaultPath = "C:\\Users\\azt12\\OneDrive\\Documents\\Obsidian Vault";

static std::string LeafOf(const std::string& full) {
    size_t s = full.find_last_of("\\/");
    return (s == std::string::npos) ? full : full.substr(s + 1);
}

static void EnsureDirectory(const std::wstring& path) {
    ::CreateDirectoryW(path.c_str(), nullptr);
}

// Percent-encode a UTF-8 string for the obsidian:// URL.
static std::string UrlEncode(const std::string& s) {
    auto is_unreserved = [](unsigned char c) {
        return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
               (c >= '0' && c <= '9') ||
               c == '-' || c == '_' || c == '.' || c == '~';
    };
    std::string out;
    for (unsigned char c : s) {
        if (is_unreserved(c)) out += (char)c;
        else {
            char buf[5];
            snprintf(buf, sizeof(buf), "%%%02X", c);
            out += buf;
        }
    }
    return out;
}

// Register a RunOnce key under HKCU that deletes `dst_path` next logon.
// Belt-and-suspenders cleanup in case the watcher process dies before
// it can do its job (system shutdown, kill, etc.).
static void RegisterRunOnceDelete(const std::wstring& dst_path) {
    HKEY hKey = nullptr;
    LONG lr = ::RegOpenKeyExW(
        HKEY_CURRENT_USER,
        L"Software\\Microsoft\\Windows\\CurrentVersion\\RunOnce",
        0, KEY_SET_VALUE, &hKey);
    if (lr != ERROR_SUCCESS || !hKey) return;
    std::wstring name = L"RCMObsidianTempCleanup_" +
                        std::to_wstring(::GetTickCount());
    std::wstring cmd  = L"cmd /c del /F /Q \"" + dst_path + L"\"";
    ::RegSetValueExW(hKey, name.c_str(), 0, REG_SZ,
                     (const BYTE*)cmd.c_str(),
                     (DWORD)((cmd.size() + 1) * sizeof(wchar_t)));
    ::RegCloseKey(hKey);
}

static void OpenInObsidianFlow(const std::string& src_path) {
    if (src_path.empty()) return;

    // 1. Ensure Temp Obsidian Vault exists as a recognizable vault.
    std::wstring wvault = widen(kTempVaultPath);
    EnsureDirectory(wvault);
    EnsureDirectory(wvault + L"\\.obsidian");

    // 2. Copy the source .md into the temp vault.
    std::string leaf = LeafOf(src_path);
    std::string dst  = std::string(kTempVaultPath) + "\\" + leaf;
    std::wstring wsrc = widen(src_path);
    std::wstring wdst = widen(dst);
    ::CopyFileW(wsrc.c_str(), wdst.c_str(), FALSE);  // overwrite OK

    // 3. Open via obsidian:// URL scheme so the right vault opens.
    //    Obsidian opens a new window for this vault even if its process
    //    is already running for the user's main vault — both windows
    //    share one process but each has its own HWND + title.
    std::string url = "obsidian://open?vault=" +
                      UrlEncode("Temp Obsidian Vault") +
                      "&file=" + UrlEncode(leaf);
    ::ShellExecuteA(nullptr, "open", url.c_str(), nullptr, nullptr,
                    SW_SHOWNORMAL);

    // 4. Spawn detached obs_watcher.exe — a small native helper that
    //    uses SetWinEventHook to listen for the SPECIFIC Obsidian
    //    window's destroy event. No polling, no busy waiting; the OS
    //    only calls our hook when window events actually fire.
    //    Args: <file-to-delete> <leaf-no-ext> "Temp Obsidian Vault"
    //    Watcher identifies our window by Obsidian's title format
    //    `<filename> - <vault name> - Obsidian`.
    std::string leaf_no_ext = leaf;
    if (leaf_no_ext.size() > 3 &&
        _stricmp(leaf_no_ext.c_str() + leaf_no_ext.size() - 3, ".md")
            == 0) {
        leaf_no_ext.resize(leaf_no_ext.size() - 3);
    }
    std::wstring wleaf_no_ext = widen(leaf_no_ext);

    // Locate obs_watcher.exe next to this menu.dll (build copies both
    // into WORKING\build\). If menu.dll moves, the watcher moves with
    // it, so derive the path from the current module's location.
    wchar_t module_path[MAX_PATH] = {};
    HMODULE hmod = nullptr;
    ::GetModuleHandleExW(
        GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
        GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
        (LPCWSTR)&OpenInObsidianFlow, &hmod);
    ::GetModuleFileNameW(hmod, module_path, MAX_PATH);
    std::wstring watcher = module_path;
    size_t bs = watcher.find_last_of(L"\\/");
    if (bs != std::wstring::npos) watcher.resize(bs);
    watcher += L"\\obs_watcher.exe";

    // Build the command line. Quote each arg in case of spaces.
    std::wstring cmdline =
        L"\"" + watcher + L"\" "
        L"\"" + wdst + L"\" "
        L"\"" + wleaf_no_ext + L"\" "
        L"\"Temp Obsidian Vault\"";

    STARTUPINFOW si = { sizeof(si) };
    PROCESS_INFORMATION pi = {};
    if (::CreateProcessW(nullptr, cmdline.data(), nullptr, nullptr,
                         FALSE,
                         CREATE_NO_WINDOW | DETACHED_PROCESS |
                         CREATE_BREAKAWAY_FROM_JOB,
                         nullptr, nullptr, &si, &pi)) {
        ::CloseHandle(pi.hProcess);
        ::CloseHandle(pi.hThread);
    }

    // 5. Fallback cleanup on next logon if watcher dies before
    //    the window closes (shutdown, crash, etc.).
    RegisterRunOnceDelete(wdst);
}

// Move (not copy) the .md file to the user's main Obsidian vault.
// Destination keeps the original filename; if a file by that name
// exists at the destination, it gets overwritten (MOVEFILE_REPLACE_EXISTING).
static void MoveToMainVault(const std::string& src_path) {
    if (src_path.empty()) return;
    std::wstring wmain = widen(kMainVaultPath);
    EnsureDirectory(wmain);

    std::string leaf  = LeafOf(src_path);
    std::wstring wsrc = widen(src_path);
    std::wstring wdst = wmain + L"\\" + widen(leaf);
    ::MoveFileExW(wsrc.c_str(), wdst.c_str(),
                  MOVEFILE_REPLACE_EXISTING);
}


// ─────────────────────────────────────────────────────────────────────
// Menu definition — macros only.
// ─────────────────────────────────────────────────────────────────────

// CommonFileOps — the shared tail of both the Folder menu and the
// default File menu. Callers decide what header items to add before
// invoking it (Open + new tab/window/pin for folders; just Open for
// files) and whether to insert a leading Separator. Lives outside
// RegisterMenu because the macros expand into nested lambdas, which
// the compiler can't reconcile when wrapped in another lambda.
static void CommonFileOps() {
    Selection("Create Desktop Shortcut", "", "create_desktop_shortcut.png"){ CreateDesktopShortcut(path);   };

    Separator();

    Selection("Copy as Path",        "", "copy_as_path.png")               { CopyAsPathViaShell(path);      };
    Selection("Rename", "F2",            "rename.png")                     { SendKeys(VK_F2);               };

    Separator();

    Selection("Copy",   "Ctrl+C",        "copy.png")                       { SendKeys('C', /*ctrl=*/true);  };

    Separator();

    Selection("Cut",    "Ctrl+X",        "cut.png")                        { SendKeys('X', /*ctrl=*/true);  };
    Selection("Delete", "Del",           "delete.png")                     { SendKeys(VK_DELETE);           };

    Separator();

    Selection("Properties", "",          "properties.png")                 { SendAltEnter();                };
}

void RegisterMenu() {
    if (target == ClickTarget::Folder) {
        Selection("Open",                "", "open.png")                       { NavigateHere(path);            };
        Selection("Open in New Tab",     "", "open_in_new_tab.png")            { OpenInNewTab(path);            };
        Selection("Open in New Window",  "", "open_in_new_window.png")         { OpenInNewWindow(path);         };

        Separator();

        Selection("Pin to Quick Access", "", "pin_to_quick_access.png")        { InvokeVerb("pintohome", path); };
        Selection("Create Desktop Shortcut", "", "create_desktop_shortcut.png"){ CreateDesktopShortcut(path);   };

        Separator();

        // Compress is Folder-only in the default menu (single file
        // gets no Compress; multi-selection gets it back in its own
        // branch below).
        Selection("Compress (Zip File)", "", "compress.png")                   { CompressToZip(path);           };

        Separator();

        // CommonFileOps starts at Create Desktop Shortcut — for the
        // folder branch we already put that line above, so we want
        // the rest. Quick inline tail (Copy as Path through Properties).
        Selection("Copy as Path",        "", "copy_as_path.png")               { CopyAsPathViaShell(path);      };
        Selection("Rename", "F2",            "rename.png")                     { SendKeys(VK_F2);               };

        Separator();

        Selection("Copy",   "Ctrl+C",        "copy.png")                       { SendKeys('C', /*ctrl=*/true);  };

        Separator();

        Selection("Cut",    "Ctrl+X",        "cut.png")                        { SendKeys('X', /*ctrl=*/true);  };
        Selection("Delete", "Del",           "delete.png")                     { SendKeys(VK_DELETE);           };

        Separator();

        Selection("Properties", "",          "properties.png")                 { SendAltEnter();                };
        return;
    }

    if (target == ClickTarget::File) {
        // ─────────────────────────────────────────────────────────────
        // Per-extension overrides — add custom branches above the
        // default. Each branch must build its full menu and `return`.
        // If no branch matches, falls through to the DEFAULT FILE MENU
        // at the bottom of this block.
        //
        //   if (extension == ".pdf") {
        //       Selection("Sign with Acrobat", "", "open.png") { ... };
        //       CommonFileOps();
        //       return;
        //   }
        //
        //   if (extension_exists({".jpg", ".png", ".webp"})) {
        //       Selection("Edit in Paint",     "", "open.png") { ... };
        //       CommonFileOps();
        //       return;
        //   }
        // ─────────────────────────────────────────────────────────────

        if (extension == ".md") {
            Selection("Open in Obsidian",  "", "open.png")    { OpenInObsidianFlow(path); };
            Selection("Move to Main Vault","", "default.png") { MoveToMainVault(path);    };
            Separator();
            CommonFileOps();
            return;
        }

        // === DEFAULT FILE MENU (fallback for any file extension) ===
        Selection("Open", "", "open.png")                                      { InvokeVerb("open", path);      };

        Separator();

        CommonFileOps();
        return;
    }

    if (target == ClickTarget::MultiSelection) {
        // Multi-selection menu: only operations that make sense when
        // more than one filesystem object is selected. Compress is the
        // headline feature here — it bundles every selected item into
        // a single Archive.zip (placed in the parent folder of the
        // first item).
        Selection("Compress (Zip File)", "", "compress.png")                   { CompressMultipleToZip(paths);  };

        Separator();

        Selection("Copy",   "Ctrl+C",        "copy.png")                       { SendKeys('C', /*ctrl=*/true);  };

        Separator();

        Selection("Cut",    "Ctrl+X",        "cut.png")                        { SendKeys('X', /*ctrl=*/true);  };
        Selection("Delete", "Del",           "delete.png")                     { SendKeys(VK_DELETE);           };

        Separator();

        Selection("Properties", "",          "properties.png")                 { SendAltEnter();                };
        return;
    }

    if (target == ClickTarget::DirectoryBackground) {
        Submenu("View",     "view.png") {
            Selection("Large icons",        "", "default.png") { set_view_mode(ViewMode::LargeIcons);  };
            Selection("Small icons",        "", "default.png") { set_view_mode(ViewMode::MediumIcons); };
            Selection("List",               "", "default.png") { set_view_mode(ViewMode::List);        };
            Selection("List with Details",  "", "default.png") { set_view_mode(ViewMode::Details);     };
        };

        Submenu("Sort by",  "sort_by.png") {
            Selection("Name",               "", "default.png") { set_sort_by(SortKey::Name);           };
            Selection("Date modified",      "", "default.png") { set_sort_by(SortKey::DateModified);   };
            Selection("Size",               "", "default.png") { set_sort_by(SortKey::Size);           };
            Selection("Type",               "", "default.png") { set_sort_by(SortKey::Type);           };
        };

        Submenu("Group by", "group_by.png") {
            Selection("Name",               "", "default.png") { set_group_by(GroupKey::Name);         };
            Selection("Date modified",      "", "default.png") { set_group_by(GroupKey::DateModified); };
            Selection("Type",               "", "default.png") { set_group_by(GroupKey::Type);         };
            Selection("(None)",             "", "default.png") { set_group_by(GroupKey::None);         };
        };

        Separator();

        Selection("Refresh", "F5", "refresh.png")                        { SendKeys(VK_F5);                  };

        Separator();

        Selection("Paste",   "Ctrl+V",        "paste.png")               { SendKeys('V', /*ctrl=*/true);     };

        Separator();

        Selection("Open in Terminal\xE2\x80\x84" "(Admin)", "", "open_in_terminal.png") { OpenTerminalAtAsAdmin(parentFolder); };
        // U+2004 THREE-PER-EM SPACE (UTF-8: E2 80 84) — wider than the
        // regular ASCII space, narrower than EN SPACE. Exo 2 kerns
        // lowercase-uppercase pairs tightly around a normal space,
        // making "New Folder" and "New Text File" look mashed.
        Selection("New\xE2\x80\x84" "Folder",                    "Ctrl+Shift+N", "new_folder.png") { SendKeys('N', true, true);     };
        Selection("New\xE2\x80\x84" "Text\xE2\x80\x84" "File", "", "new_text_file.png")            { CreateNewTextFile(parentFolder); };

        Separator();

        Selection("Properties", "",           "properties.png")          { SendAltEnter();                   };
    }
}
