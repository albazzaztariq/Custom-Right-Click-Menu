// shell_context.cpp
// Walk IShellWindows to find the explorer window the click belongs to,
// pull its IShellView, extract folder + selection, and feed them into
// the context globals.
//
// Pattern is the standard Raymond Chen recipe:
//   IShellWindows -> Item(i) -> IDispatch
//     -> IServiceProvider::QueryService(SID_STopLevelBrowser, IShellBrowser)
//     -> QueryActiveShellView -> IShellView
//     -> QI IFolderView
//     -> Items(SVGIO_SELECTION, IID_IShellItemArray, ...)

#include "shell_context.h"
#include "context.h"
#include "menu_api.h"
#include "diag.h"

#include <shlobj.h>
#include <shlwapi.h>
#include <exdisp.h>
#include <shobjidl.h>
#include <string>
#include <vector>

#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "shlwapi.lib")
#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "oleaut32.lib")

namespace {

template <class T>
void safe_release(T*& p) {
    if (p) { p->Release(); p = nullptr; }
}

std::string narrow_utf8(const wchar_t* w) {
    if (!w || !*w) return {};
    int n = ::WideCharToMultiByte(CP_UTF8, 0, w, -1,
                                  nullptr, 0, nullptr, nullptr);
    if (n <= 1) return {};
    std::string s(static_cast<size_t>(n - 1), '\0');
    ::WideCharToMultiByte(CP_UTF8, 0, w, -1, s.data(), n, nullptr, nullptr);
    return s;
}

void read_modifiers() {
    bool shift = (GetKeyState(VK_SHIFT)   & 0x8000) != 0;
    bool ctrl  = (GetKeyState(VK_CONTROL) & 0x8000) != 0;
    bool alt   = (GetKeyState(VK_MENU)    & 0x8000) != 0;
    bool win   = (GetKeyState(VK_LWIN)    & 0x8000) != 0
              || (GetKeyState(VK_RWIN)    & 0x8000) != 0;
    _menu_internal::set_modifiers(shift, ctrl, alt, win);
}

// Walk IShellWindows looking for one whose top-level HWND matches
// the click. Match strategy, in order:
//   1) hw == GA_ROOT(owner_hwnd)
//   2) hw == GA_ROOTOWNER(owner_hwnd)
//   3) hw's window rect contains the click point in screen coords
//
// On Win11 24H2 the modern-menu surface is an unrelated top-level
// popup with no parent OR owner relationship to the Cabinet frame,
// so neither GA_ROOT nor GA_ROOTOWNER reaches the right HWND. The
// point-containment check is the only robust fallback: the user
// right-clicked inside an explorer window, so its rect contains the
// click coords. Caller owns the returned IShellBrowser*.
IShellBrowser* find_shell_browser_for(HWND owner_top, HWND owner_rootowner,
                                      int click_x, int click_y,
                                      bool& is_desktop) {
    is_desktop = false;
    IShellWindows* psw = nullptr;
    // CLSCTX_ALL prefers any in-proc handler before falling back to
    // the local-server path. The local-server activation is ~hundreds
    // of ms cold, which was triggering the system busy cursor before
    // our menu had a chance to grab capture.
    HRESULT hr = ::CoCreateInstance(CLSID_ShellWindows, nullptr,
                                    CLSCTX_ALL,
                                    IID_PPV_ARGS(&psw));
    if (FAILED(hr) || !psw) {
        diag::log("find_shell_browser_for: CoCreateInstance(ShellWindows) "
                  "failed hr=0x%08x", hr);
        return nullptr;
    }

    long count = 0;
    psw->get_Count(&count);
    IShellBrowser* match = nullptr;

    for (long i = 0; i < count && !match; ++i) {
        VARIANT vi; ::VariantInit(&vi);
        vi.vt = VT_I4; vi.lVal = i;
        IDispatch* disp = nullptr;
        if (FAILED(psw->Item(vi, &disp)) || !disp) continue;

        IServiceProvider* sp = nullptr;
        if (SUCCEEDED(disp->QueryInterface(IID_PPV_ARGS(&sp))) && sp) {
            IShellBrowser* sb = nullptr;
            if (SUCCEEDED(sp->QueryService(SID_STopLevelBrowser,
                                           IID_PPV_ARGS(&sb))) && sb) {
                HWND hw = nullptr;
                HRESULT ghr = sb->GetWindow(&hw);
                bool m = false;
                bool pt_match = false;
                if (SUCCEEDED(ghr) && hw) {
                    m = (hw == owner_top || hw == owner_rootowner);
                    if (!m) {
                        RECT wr = {};
                        if (::GetWindowRect(hw, &wr)) {
                            POINT pt = { click_x, click_y };
                            if (::PtInRect(&wr, pt)) {
                                m = true;
                                pt_match = true;
                            }
                        }
                    }
                }
                diag::log("  fsb iter %ld: GetWindow hr=0x%08x hw=%p "
                          "owner_top=%p owner_rootowner=%p click=(%d,%d) "
                          "match=%d pt_match=%d",
                          i, ghr, hw, owner_top, owner_rootowner,
                          click_x, click_y, m, pt_match);
                if (m) {
                    match = sb;  // transfer ref
                } else {
                    safe_release(sb);
                }
            }
            safe_release(sp);
        }
        safe_release(disp);
    }
    diag::log("find_shell_browser_for: post-loop match=%p count=%ld",
              (void*)match, count);

    // If nothing matched and the click is on the shell desktop, the
    // desktop's "window" is the progman/workerw HWND. IShellWindows
    // exposes it under a sentinel index that varies by Windows build;
    // detect by class name instead.
    if (!match) {
        wchar_t cls[64] = {};
        ::GetClassNameW(owner_top, cls, 64);
        if (::lstrcmpiW(cls, L"Progman") == 0
         || ::lstrcmpiW(cls, L"WorkerW") == 0) {
            is_desktop = true;
        }
    }

    safe_release(psw);
    return match;
}

std::string path_from_pidl(LPCITEMIDLIST pidl) {
    if (!pidl) return {};
    wchar_t buf[MAX_PATH] = {};
    if (::SHGetPathFromIDListW(pidl, buf)) {
        return narrow_utf8(buf);
    }
    return {};
}

std::string path_from_shell_item(IShellItem* item) {
    if (!item) return {};
    LPWSTR w = nullptr;
    if (FAILED(item->GetDisplayName(SIGDN_FILESYSPATH, &w)) || !w) {
        return {};
    }
    std::string s = narrow_utf8(w);
    ::CoTaskMemFree(w);
    return s;
}

ClickTarget infer_single_target(const std::string& p, bool background) {
    if (background) return ClickTarget::DirectoryBackground;
    if (p.empty())  return ClickTarget::VirtualItem;

    // Drive: "X:\" — 3 chars, second is colon, third is slash.
    if (p.size() == 3 && p[1] == ':'
        && (p[2] == '\\' || p[2] == '/')) {
        return ClickTarget::Drive;
    }

    std::wstring wp(p.begin(), p.end());
    DWORD attrs = ::GetFileAttributesW(wp.c_str());
    diag::log("infer_single_target: p='%s' attrs=0x%08x gle=%lu",
              p.c_str(), attrs, attrs == INVALID_FILE_ATTRIBUTES
                                 ? ::GetLastError() : 0UL);
    if (attrs == INVALID_FILE_ATTRIBUTES) return ClickTarget::VirtualItem;
    if (attrs & FILE_ATTRIBUTE_DIRECTORY) return ClickTarget::Folder;
    return ClickTarget::File;
}

}  // namespace


bool populate_shell_context(HWND owner_hwnd, int screen_x, int screen_y) {
    _menu_internal::reset_context();
    _menu_internal::set_click_location(screen_x, screen_y);
    read_modifiers();

    // COINIT — explorer's UI thread is STA, so this is usually a no-op
    // (returns S_FALSE or RPC_E_CHANGED_MODE). Either is fine.
    HRESULT co_hr = ::CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    bool need_couninit = (co_hr == S_OK);

    HWND owner_top       = owner_hwnd ? ::GetAncestor(owner_hwnd, GA_ROOT)
                                      : nullptr;
    HWND owner_rootowner = owner_hwnd ? ::GetAncestor(owner_hwnd, GA_ROOTOWNER)
                                      : nullptr;
    diag::log("populate_shell_context: owner=%p top=%p rootowner=%p",
              owner_hwnd, owner_top, owner_rootowner);

    bool is_desktop = false;
    IShellBrowser* sb = find_shell_browser_for(owner_top, owner_rootowner,
                                               screen_x, screen_y,
                                               is_desktop);
    diag::log("populate_shell_context: co_hr=0x%08x sb=%p is_desktop=%d",
              co_hr, (void*)sb, is_desktop);

    bool ok = false;

    if (!sb && is_desktop) {
        _menu_internal::set_target(ClickTarget::Desktop);
        // Desktop background: parentFolder = user's Desktop path.
        wchar_t desk[MAX_PATH] = {};
        if (SUCCEEDED(::SHGetFolderPathW(nullptr, CSIDL_DESKTOPDIRECTORY,
                                         nullptr, 0, desk))) {
            std::vector<std::string> empty;
            _menu_internal::set_paths(empty);
            // parentFolder isn't directly exposed on set_paths when
            // the list is empty; set it via a single-item probe.
            // Simpler: just set target + leave paths empty. Consumer
            // checks target == Desktop and uses CSIDL itself if needed.
        }
        ok = true;
    }

    if (sb) {
        IShellView* sv = nullptr;
        if (SUCCEEDED(sb->QueryActiveShellView(&sv)) && sv) {
            // Selection enumeration via IFolderView.
            IFolderView* fv = nullptr;
            std::vector<std::string> sel_paths;

            std::string folder_path;
            if (SUCCEEDED(sv->QueryInterface(IID_PPV_ARGS(&fv))) && fv) {
                // Folder PIDL + IShellFolder first — we need the parent
                // IShellFolder for the focused-item fallback path
                // below to resolve a relative PIDL into a full path.
                IShellFolder* parent_isf = nullptr;
                if (SUCCEEDED(fv->GetFolder(IID_IShellFolder,
                                            (void**)&parent_isf))
                    && parent_isf) {
                    IPersistFolder2* pf2 = nullptr;
                    if (SUCCEEDED(parent_isf->QueryInterface(
                            IID_PPV_ARGS(&pf2))) && pf2) {
                        LPITEMIDLIST pidl = nullptr;
                        if (SUCCEEDED(pf2->GetCurFolder(&pidl)) && pidl) {
                            folder_path = path_from_pidl(pidl);
                            ::CoTaskMemFree(pidl);
                        }
                        safe_release(pf2);
                    }
                }

                // Primary: enumerate SVGIO_SELECTION.
                IShellItemArray* items = nullptr;
                HRESULT hr = fv->Items(SVGIO_SELECTION,
                                       IID_PPV_ARGS(&items));
                DWORD svgio_count = 0;
                if (SUCCEEDED(hr) && items) {
                    items->GetCount(&svgio_count);
                    for (DWORD i = 0; i < svgio_count; ++i) {
                        IShellItem* it = nullptr;
                        if (SUCCEEDED(items->GetItemAt(i, &it)) && it) {
                            std::string p = path_from_shell_item(it);
                            diag::log("  sel[%lu] path='%s'", i, p.c_str());
                            if (!p.empty()) sel_paths.push_back(p);
                            safe_release(it);
                        }
                    }
                    safe_release(items);
                }
                diag::log("  fv->Items(SVGIO_SELECTION) hr=0x%08x "
                          "count=%lu sel_paths=%zu",
                          hr, svgio_count, sel_paths.size());

                // (Earlier we added a GetFocusedItem fallback for the
                // case "user right-clicked an item but SVGIO_SELECTION
                // returned 0". That fallback misfired on whitespace
                // right-clicks too, because the previously-focused
                // item is still "focused" even when the click missed
                // every item. Removed. If the SVGIO_SELECTION==0
                // case ever recurs on a real item click, the correct
                // fix is hit-testing the click point against item
                // rects, not blindly trusting the focused index.)

                safe_release(parent_isf);
                safe_release(fv);
            }

            if (sel_paths.empty()) {
                _menu_internal::set_target(ClickTarget::DirectoryBackground);
                // Use a single-item paths trick to surface parentFolder
                // through the existing set_paths plumbing? No — paths
                // must reflect selection. Set folder_path via direct
                // assignment to the global instead.
                if (!folder_path.empty()) {
                    parentFolder = folder_path;
                }
            } else if (sel_paths.size() == 1) {
                _menu_internal::set_paths(sel_paths);
                _menu_internal::set_target(
                    infer_single_target(sel_paths[0], false));
                if (!folder_path.empty()) {
                    parentFolder = folder_path;
                }
            } else {
                _menu_internal::set_paths(sel_paths);
                _menu_internal::set_target(ClickTarget::MultiSelection);
                if (!folder_path.empty()) {
                    parentFolder = folder_path;
                }
            }

            diag::log("populate_shell_context: target=%d sel=%zu folder=%s",
                      (int)target, sel_paths.size(), folder_path.c_str());
            ok = true;
            safe_release(sv);
        }
        safe_release(sb);
    }

    if (need_couninit) ::CoUninitialize();
    return ok;
}
