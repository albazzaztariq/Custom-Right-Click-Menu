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
#include <shldisp.h>
#include <commctrl.h>
#include <oleacc.h>
#include <uiautomation.h>
#include <KnownFolders.h>
#include <string>
#include <vector>

#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "shlwapi.lib")
#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "oleaut32.lib")
#pragma comment(lib, "oleacc.lib")

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

// Per-tab subtree helpers (multi-LLM consult, 2026-05-22).
//
// On Win11 24H2 tabbed Explorer there is ONE IShellBrowser per cabinet
// for the whole window (count==1 in IShellWindows). All tabs share it,
// and IShellBrowser::QueryActiveShellView returns the view of whichever
// tab Explorer treats as currently-active — which is the original tab,
// not the one the user just right-clicked in. The wrong tab's selection
// and folder come back.
//
// Workaround per a consensus from ask-claude + ask-grok + qwen: anchor
// on the ShellTabWindowClass HWND we already get from walking up from
// owner_hwnd, then:
//   Tier 1: SHGetShellItemForWindow(tab_hwnd) — documented API that
//           returns the IShellItem for the folder THIS tab is showing.
//   Tier 2: EnumChildWindows down to that tab's own SysListView32, then
//           LVM_GETNEXTITEM(LVNI_SELECTED) + LVM_GETITEM(LVIF_PARAM)
//           for per-item PIDLs. Each tab owns its own listview subtree
//           even though the COM layer is unified across tabs.

struct find_descendant_ctx {
    const wchar_t* target_class;
    HWND result;
};

static BOOL CALLBACK find_descendant_proc(HWND hwnd, LPARAM lp) {
    auto* ctx = reinterpret_cast<find_descendant_ctx*>(lp);
    wchar_t buf[64] = {};
    ::GetClassNameW(hwnd, buf, 64);
    if (wcscmp(buf, ctx->target_class) == 0) {
        ctx->result = hwnd;
        return FALSE;
    }
    return TRUE;
}

static HWND find_descendant_by_class(HWND root, const wchar_t* cls) {
    if (!root) return nullptr;
    find_descendant_ctx ctx = { cls, nullptr };
    ::EnumChildWindows(root, find_descendant_proc,
                       reinterpret_cast<LPARAM>(&ctx));
    return ctx.result;
}

// Diagnostic: probe a single HWND with AccObj→IServiceProvider→
// QueryService(various SIDs). Only fires for "interesting" classes.
static void probe_one(HWND hwnd) {
    wchar_t cls[64] = {};
    ::GetClassNameW(hwnd, cls, 64);
    bool interesting =
        (wcscmp(cls, L"SHELLDLL_DefView") == 0)
     || (wcscmp(cls, L"DirectUIHWND") == 0)
     || (wcscmp(cls, L"ShellTabContentClass") == 0)
     || (wcscmp(cls, L"DUIViewWndClassName") == 0)
     || (wcscmp(cls, L"ItemsView") == 0)
     || (wcscmp(cls, L"ModernCollectionBasePanel") == 0)
     || (wcscmp(cls, L"NamespaceTreeControl") == 0);
    if (!interesting) return;

    char cls_a[64] = {};
    ::WideCharToMultiByte(CP_UTF8, 0, cls, -1, cls_a, 64, nullptr, nullptr);

    // Path 1: OBJID_WINDOW → IServiceProvider → QueryService(SID_S*Browser)
    IServiceProvider* sp = nullptr;
    HRESULT hr = ::AccessibleObjectFromWindow(
        hwnd, (DWORD)OBJID_WINDOW,
        IID_IServiceProvider, (void**)&sp);
    diag::log("      probe hwnd=%p class='%s' AccObj(OBJID_WINDOW) "
              "hr=0x%08x sp=%p", hwnd, cls_a, hr, (void*)sp);
    if (SUCCEEDED(hr) && sp) {
        struct sid_entry { const GUID* sid; const char* name; };
        const sid_entry sids[] = {
            { &SID_SShellBrowser,    "SID_SShellBrowser"    },
            { &SID_STopLevelBrowser, "SID_STopLevelBrowser" },
            { &SID_SInPlaceBrowser,  "SID_SInPlaceBrowser"  },
        };
        for (const auto& s : sids) {
            IShellBrowser* psb = nullptr;
            HRESULT qhr = sp->QueryService(*s.sid, IID_PPV_ARGS(&psb));
            if (psb) {
                IShellView* psv = nullptr;
                HWND psv_hwnd = nullptr;
                if (SUCCEEDED(psb->QueryActiveShellView(&psv)) && psv) {
                    psv->GetWindow(&psv_hwnd);
                    psv->Release();
                }
                diag::log("        QS %s hr=0x%08x psb=%p psv_hwnd=%p",
                          s.name, qhr, (void*)psb, psv_hwnd);
                psb->Release();
            } else {
                diag::log("        QS %s hr=0x%08x psb=NULL",
                          s.name, qhr);
            }
        }
        sp->Release();
    }

    // Path 2: OBJID_NATIVEOM → IDispatch → QI IShellFolderViewDual.
    // This is the Raymond Chen "automation object for the shell view"
    // path — the one path we hadn't actually exercised on SHELLDLL_DefView.
    IDispatch* disp = nullptr;
    HRESULT hr2 = ::AccessibleObjectFromWindow(
        hwnd, (DWORD)OBJID_NATIVEOM,
        IID_IDispatch, (void**)&disp);
    diag::log("      probe hwnd=%p AccObj(OBJID_NATIVEOM, IDispatch) "
              "hr=0x%08x disp=%p", hwnd, hr2, (void*)disp);
    if (SUCCEEDED(hr2) && disp) {
        IShellFolderViewDual* sfvd = nullptr;
        HRESULT qhr = disp->QueryInterface(IID_PPV_ARGS(&sfvd));
        diag::log("        QI IShellFolderViewDual hr=0x%08x sfvd=%p",
                  qhr, (void*)sfvd);
        if (sfvd) {
            // Touch get_Folder to confirm it actually responds with
            // the tab's own folder, not the active tab's.
            Folder* folder = nullptr;
            if (SUCCEEDED(sfvd->get_Folder(&folder)) && folder) {
                Folder2* folder2 = nullptr;
                if (SUCCEEDED(folder->QueryInterface(
                        IID_PPV_ARGS(&folder2))) && folder2) {
                    FolderItem* self_item = nullptr;
                    if (SUCCEEDED(folder2->get_Self(&self_item))
                        && self_item) {
                        BSTR path = nullptr;
                        if (SUCCEEDED(self_item->get_Path(&path)) && path) {
                            char buf[512] = {};
                            ::WideCharToMultiByte(CP_UTF8, 0, path, -1,
                                buf, sizeof(buf), nullptr, nullptr);
                            diag::log("        sfvd folder path='%s'", buf);
                            ::SysFreeString(path);
                        }
                        self_item->Release();
                    }
                    folder2->Release();
                }
                folder->Release();
            }
            sfvd->Release();
        }
        disp->Release();
    }
}

// Recursive enum callback: dump hwnd+class+rect, then probe if class
// looks interesting.
static BOOL CALLBACK dump_and_probe_proc(HWND hwnd, LPARAM /*lp*/) {
    wchar_t cls[64] = {};
    ::GetClassNameW(hwnd, cls, 64);
    char cls_a[64] = {};
    ::WideCharToMultiByte(CP_UTF8, 0, cls, -1, cls_a, 64, nullptr, nullptr);
    RECT r = {};
    ::GetWindowRect(hwnd, &r);
    diag::log("    dump hwnd=%p class='%s' rect=(%ld,%ld %ldx%ld)",
              hwnd, cls_a, r.left, r.top,
              r.right - r.left, r.bottom - r.top);
    probe_one(hwnd);
    return TRUE;
}

// One-shot diagnostic dump: full descendant tree of active_tab and
// of its containing CabinetWClass (which exposes the sibling chain
// where DirectUIHWND / ShellTabContentClass may live).
static void diagnostic_dump_tab(HWND active_tab) {
    if (!active_tab) return;

    diag::log("=== diagnostic dump for active_tab=%p ===", active_tab);

    // Walk up to CabinetWClass.
    HWND cabinet = nullptr;
    {
        HWND h = active_tab;
        for (int i = 0; h && i < 32; ++i) {
            wchar_t cls[64] = {};
            ::GetClassNameW(h, cls, 64);
            if (wcscmp(cls, L"CabinetWClass") == 0) {
                cabinet = h;
                break;
            }
            h = ::GetParent(h);
        }
    }
    diag::log("  cabinet=%p", cabinet);

    diag::log("  --- descendants of active_tab ---");
    ::EnumChildWindows(active_tab, dump_and_probe_proc, 0);

    if (cabinet && cabinet != active_tab) {
        diag::log("  --- descendants of cabinet ---");
        ::EnumChildWindows(cabinet, dump_and_probe_proc, 0);
    }
    diag::log("=== end diagnostic dump ===");
}

// Read folder + selection straight from UIA on the per-tab
// SHELLDLL_DefView, bypassing the stale per-cabinet IShellBrowser
// that's anchored to the original tab on Win11 24H2.
//
// Folder: find the currently-selected UIA TabItem under the cabinet —
// its Name property is the full folder path (verified empirically).
// Selection: UIA SelectionPattern on (or under) the active_tab's
// SHELLDLL_DefView, walk the selected child elements, get their Name
// (display name) and resolve to a real path by combining with the
// folder path.
//
// Lossy on hidden extensions and virtual items (UIA gives display
// name, not real filename). We probe `<folder>\<name>` first; if
// `GetFileAttributesW` fails, try a short list of common extensions.
static bool read_active_tab_via_uia(
        HWND active_tab,
        std::string& folder_out,
        std::vector<std::string>& sel_out) {
    if (!active_tab) return false;

    HWND cabinet = ::GetAncestor(active_tab, GA_ROOT);
    HWND def_view = find_descendant_by_class(active_tab, L"SHELLDLL_DefView");
    diag::log("  uia-read: cabinet=%p def_view=%p", cabinet, def_view);
    if (!cabinet || !def_view) return false;

    IUIAutomation* uia = nullptr;
    HRESULT hr = ::CoCreateInstance(CLSID_CUIAutomation, nullptr,
                                     CLSCTX_INPROC_SERVER,
                                     IID_PPV_ARGS(&uia));
    diag::log("  uia-read: CoCreateInstance hr=0x%08x uia=%p",
              hr, (void*)uia);
    if (FAILED(hr) || !uia) return false;

    // ── Folder: currently-selected TabItem in cabinet → Name
    IUIAutomationElement* cab_elem = nullptr;
    if (SUCCEEDED(uia->ElementFromHandle(cabinet, &cab_elem)) && cab_elem) {
        VARIANT vt; ::VariantInit(&vt);
        vt.vt = VT_I4; vt.lVal = UIA_TabItemControlTypeId;
        IUIAutomationCondition* cond = nullptr;
        uia->CreatePropertyCondition(UIA_ControlTypePropertyId, vt, &cond);
        if (cond) {
            IUIAutomationElementArray* arr = nullptr;
            cab_elem->FindAll(TreeScope_Descendants, cond, &arr);
            int n = 0;
            if (arr) arr->get_Length(&n);
            for (int i = 0; i < n; ++i) {
                IUIAutomationElement* item = nullptr;
                if (FAILED(arr->GetElement(i, &item)) || !item) continue;
                IUIAutomationSelectionItemPattern* sip = nullptr;
                item->GetCurrentPatternAs(UIA_SelectionItemPatternId,
                                           IID_PPV_ARGS(&sip));
                BOOL is_sel = FALSE;
                if (sip) { sip->get_CurrentIsSelected(&is_sel); sip->Release(); }
                if (is_sel) {
                    BSTR name = nullptr;
                    item->get_CurrentName(&name);
                    if (name) {
                        folder_out = narrow_utf8(name);
                        ::SysFreeString(name);
                    }
                    item->Release();
                    break;
                }
                item->Release();
            }
            if (arr) arr->Release();
            cond->Release();
        }
        cab_elem->Release();
    }
    // For known/library folders ("Downloads", "Documents", "Music"
    // etc.) Win11's TabItem.Name returns just the localized display
    // label, not the absolute path. Map those back to their real
    // filesystem locations via SHGetKnownFolderPath so downstream
    // path resolution works.
    if (!folder_out.empty() &&
        folder_out.find(':') == std::string::npos) {
        struct known_kv { const char* name; const KNOWNFOLDERID* id; };
        static const known_kv known[] = {
            { "Downloads",    &FOLDERID_Downloads     },
            { "Documents",    &FOLDERID_Documents     },
            { "Music",        &FOLDERID_Music         },
            { "Pictures",     &FOLDERID_Pictures      },
            { "Videos",       &FOLDERID_Videos        },
            { "Desktop",      &FOLDERID_Desktop       },
            { "3D Objects",   &FOLDERID_Objects3D     },
            { "Public",       &FOLDERID_Public        },
            { "Saved Games",  &FOLDERID_SavedGames    },
            { "Contacts",     &FOLDERID_Contacts      },
            { "Favorites",    &FOLDERID_Favorites     },
            { "Links",        &FOLDERID_Links         },
            { "Searches",     &FOLDERID_SavedSearches },
        };
        for (const auto& k : known) {
            if (folder_out == k.name) {
                PWSTR p = nullptr;
                if (SUCCEEDED(::SHGetKnownFolderPath(*k.id, 0, nullptr, &p))
                    && p) {
                    std::string resolved = narrow_utf8(p);
                    ::CoTaskMemFree(p);
                    diag::log("  uia-read: resolved known folder '%s' -> '%s'",
                              folder_out.c_str(), resolved.c_str());
                    folder_out = resolved;
                }
                break;
            }
        }
    }
    diag::log("  uia-read: folder='%s'", folder_out.c_str());

    // ── Selection: SelectionPattern on def_view (or any descendant
    // that supports it). On modern shell the listview is a
    // DirectUIHWND inside DefView and that one carries the
    // SelectionPattern.
    IUIAutomationElement* dv_elem = nullptr;
    if (SUCCEEDED(uia->ElementFromHandle(def_view, &dv_elem)) && dv_elem) {
        // Find the descendant supporting SelectionPattern.
        VARIANT vp; ::VariantInit(&vp);
        vp.vt = VT_BOOL; vp.boolVal = VARIANT_TRUE;
        IUIAutomationCondition* sp_cond = nullptr;
        uia->CreatePropertyCondition(UIA_IsSelectionPatternAvailablePropertyId,
                                      vp, &sp_cond);
        IUIAutomationElement* host = nullptr;
        if (sp_cond) {
            cab_elem = nullptr;
            dv_elem->FindFirst(
                (TreeScope)(TreeScope_Element | TreeScope_Descendants),
                sp_cond, &host);
            sp_cond->Release();
        }
        if (!host) host = dv_elem, dv_elem = nullptr; // reuse local
        diag::log("  uia-read: selection-host elem=%p", (void*)host);

        if (host) {
            IUIAutomationSelectionPattern* sp = nullptr;
            host->GetCurrentPatternAs(UIA_SelectionPatternId,
                                       IID_PPV_ARGS(&sp));
            if (sp) {
                IUIAutomationElementArray* sel_arr = nullptr;
                sp->GetCurrentSelection(&sel_arr);
                int n = 0;
                if (sel_arr) sel_arr->get_Length(&n);
                diag::log("  uia-read: selection count=%d", n);
                for (int i = 0; i < n; ++i) {
                    IUIAutomationElement* item = nullptr;
                    if (FAILED(sel_arr->GetElement(i, &item)) || !item)
                        continue;
                    BSTR name = nullptr;
                    item->get_CurrentName(&name);
                    if (name) {
                        std::string fname = narrow_utf8(name);
                        ::SysFreeString(name);
                        if (!folder_out.empty() && !fname.empty()) {
                            std::string base = folder_out + "\\" + fname;
                            std::wstring wbase(base.begin(), base.end());
                            DWORD attrs = ::GetFileAttributesW(wbase.c_str());
                            if (attrs != INVALID_FILE_ATTRIBUTES) {
                                sel_out.push_back(base);
                                diag::log("    uia-read: sel[%d] path='%s'",
                                          i, base.c_str());
                            } else {
                                // Hidden-extension probe: try common exts.
                                static const char* exts[] = {
                                    ".lnk", ".txt", ".md", ".doc",
                                    ".docx", ".pdf", ".png", ".jpg",
                                    ".jpeg", ".xlsx", ".pptx", ".zip",
                                    ".rar", ".7z", ".exe", ".bat",
                                    ".ps1", ".py", ".cpp", ".h"
                                };
                                bool resolved = false;
                                for (const char* ext : exts) {
                                    std::string cand = base + ext;
                                    std::wstring wc(cand.begin(), cand.end());
                                    if (::GetFileAttributesW(wc.c_str())
                                        != INVALID_FILE_ATTRIBUTES) {
                                        sel_out.push_back(cand);
                                        diag::log("    uia-read: sel[%d] "
                                                  "resolved path='%s'",
                                                  i, cand.c_str());
                                        resolved = true;
                                        break;
                                    }
                                }
                                if (!resolved) {
                                    sel_out.push_back(base);
                                    diag::log("    uia-read: sel[%d] "
                                              "UNRESOLVED path='%s'",
                                              i, base.c_str());
                                }
                            }
                        }
                    }
                    item->Release();
                }
                if (sel_arr) sel_arr->Release();
                sp->Release();
            }
            host->Release();
        }
        if (dv_elem) dv_elem->Release();
    }
    uia->Release();

    return !folder_out.empty();
}

// EnumChildWindows context for finding active_tab's index among
// cabinet's ShellTabWindowClass children.
struct tab_index_ctx { HWND target; int seen; int found; };

static BOOL CALLBACK tab_index_proc(HWND hwnd, LPARAM lp) {
    auto* c = reinterpret_cast<tab_index_ctx*>(lp);
    wchar_t cls[64] = {};
    ::GetClassNameW(hwnd, cls, 64);
    if (wcscmp(cls, L"ShellTabWindowClass") == 0) {
        if (hwnd == c->target) { c->found = c->seen; return FALSE; }
        c->seen++;
    }
    return TRUE;
}

// Force-activate the right-clicked tab. The previous attempt (UIA
// SetFocus on active_tab content) was a no-op for tab switching. The
// real switch lever is the tab-header chip in the title bar — a UIA
// TabItem control. We:
//   1. Find active_tab's index among cabinet's ShellTabWindowClass
//      siblings (0-based, in EnumChildWindows order).
//   2. UIA-enumerate cabinet's descendant TabItem controls.
//   3. Invoke/Select the TabItem at that index.
//   4. Pump messages so Explorer processes the switch.
static bool try_activate_tab_via_uia(HWND active_tab) {
    if (!active_tab) return false;

    HWND cabinet = ::GetAncestor(active_tab, GA_ROOT);
    if (!cabinet) return false;

    // Find active_tab's index in cabinet's tab list.
    tab_index_ctx ictx = { active_tab, 0, -1 };
    ::EnumChildWindows(cabinet, tab_index_proc,
                       reinterpret_cast<LPARAM>(&ictx));
    diag::log("  uia-activate: active_tab index=%d in cabinet=%p",
              ictx.found, cabinet);
    if (ictx.found < 0) return false;

    IUIAutomation* uia = nullptr;
    HRESULT hr = ::CoCreateInstance(CLSID_CUIAutomation, nullptr,
                                     CLSCTX_INPROC_SERVER,
                                     IID_PPV_ARGS(&uia));
    diag::log("  uia-activate: CoCreateInstance hr=0x%08x uia=%p",
              hr, (void*)uia);
    if (FAILED(hr) || !uia) return false;

    bool ok = false;
    IUIAutomationElement* cabinet_elem = nullptr;
    if (SUCCEEDED(uia->ElementFromHandle(cabinet, &cabinet_elem))
        && cabinet_elem) {
        VARIANT v; ::VariantInit(&v);
        v.vt = VT_I4; v.lVal = UIA_TabItemControlTypeId;
        IUIAutomationCondition* cond = nullptr;
        uia->CreatePropertyCondition(UIA_ControlTypePropertyId, v, &cond);
        if (cond) {
            IUIAutomationElementArray* arr = nullptr;
            cabinet_elem->FindAll(TreeScope_Descendants, cond, &arr);
            int n = 0;
            if (arr) arr->get_Length(&n);
            diag::log("  uia-activate: TabItem-descendants count=%d", n);
            if (arr && ictx.found < n) {
                IUIAutomationElement* item = nullptr;
                if (SUCCEEDED(arr->GetElement(ictx.found, &item)) && item) {
                    BSTR name = nullptr;
                    item->get_CurrentName(&name);
                    char nbuf[256] = {};
                    if (name) {
                        ::WideCharToMultiByte(CP_UTF8, 0, name, -1,
                            nbuf, sizeof(nbuf), nullptr, nullptr);
                        ::SysFreeString(name);
                    }
                    diag::log("  uia-activate: TabItem[%d] name='%s'",
                              ictx.found, nbuf);

                    // Try SelectionItemPattern.Select first — that's
                    // what TabItems implement.
                    IUIAutomationSelectionItemPattern* sel = nullptr;
                    item->GetCurrentPatternAs(UIA_SelectionItemPatternId,
                                               IID_PPV_ARGS(&sel));
                    if (sel) {
                        HRESULT shr = sel->Select();
                        diag::log("  uia-activate: TabItem.Select hr=0x%08x",
                                  shr);
                        if (SUCCEEDED(shr)) ok = true;
                        sel->Release();
                    }
                    // Fallback: InvokePattern
                    if (!ok) {
                        IUIAutomationInvokePattern* inv = nullptr;
                        item->GetCurrentPatternAs(UIA_InvokePatternId,
                                                   IID_PPV_ARGS(&inv));
                        if (inv) {
                            HRESULT ihr = inv->Invoke();
                            diag::log("  uia-activate: TabItem.Invoke "
                                      "hr=0x%08x", ihr);
                            if (SUCCEEDED(ihr)) ok = true;
                            inv->Release();
                        }
                    }
                    item->Release();
                }
            }
            if (arr) arr->Release();
            cond->Release();
        }
        cabinet_elem->Release();
    }
    uia->Release();

    if (ok) {
        // Pump briefly so Explorer processes the tab switch.
        MSG msg;
        DWORD start = ::GetTickCount();
        while (::GetTickCount() - start < 120) {
            while (::PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
                ::TranslateMessage(&msg);
                ::DispatchMessageW(&msg);
            }
            ::Sleep(5);
        }
    }
    return ok;
}

void read_modifiers() {
    bool shift = (GetKeyState(VK_SHIFT)   & 0x8000) != 0;
    bool ctrl  = (GetKeyState(VK_CONTROL) & 0x8000) != 0;
    bool alt   = (GetKeyState(VK_MENU)    & 0x8000) != 0;
    bool win   = (GetKeyState(VK_LWIN)    & 0x8000) != 0
              || (GetKeyState(VK_RWIN)    & 0x8000) != 0;
    _menu_internal::set_modifiers(shift, ctrl, alt, win);
}

// Walk IShellWindows looking for the IShellBrowser belonging to the
// SPECIFIC TAB the user right-clicked in. Match strategy, in order:
//   0) Per-TAB: walk up from owner_hwnd to find the active
//      ShellTabWindowClass; the IShellBrowser whose shell-view HWND
//      is a descendant of THAT tab is the one we want.
//   1) hw == GA_ROOT(owner_hwnd)
//   2) hw == GA_ROOTOWNER(owner_hwnd)
//   3) hw's window rect contains the click point in screen coords
//
// On Win11 24H2 tabbed Explorer each tab has its own IShellBrowser
// in IShellWindows, but IShellBrowser::GetWindow() returns the
// CABINET HWND for ALL tabs (they share the outer frame), so matching
// by that HWND would always grab the first tab. The active tab is
// distinguishable only by its per-tab shell-view HWND, which lives
// inside that tab's ShellTabWindowClass subtree.
//
// On Win11 24H2 the modern-menu surface is an unrelated top-level
// popup with no parent OR owner relationship to the Cabinet frame,
// so neither GA_ROOT nor GA_ROOTOWNER reaches the right HWND. The
// point-containment check is the only robust fallback: the user
// right-clicked inside an explorer window, so its rect contains the
// click coords. Caller owns the returned IShellBrowser*.
IShellBrowser* find_shell_browser_for(HWND owner_hwnd,
                                      HWND owner_top, HWND owner_rootowner,
                                      int click_x, int click_y,
                                      bool& is_desktop) {
    is_desktop = false;

    // Walk up the click HWND looking for the specific tab container.
    HWND active_tab = nullptr;
    {
        HWND h = owner_hwnd;
        for (int hops = 0; h && hops < 32; ++hops) {
            wchar_t cls[64] = {};
            ::GetClassNameW(h, cls, 64);
            char cls_a[64] = {};
            ::WideCharToMultiByte(CP_UTF8, 0, cls, -1, cls_a, 64, nullptr, nullptr);
            diag::log("  fsb walk hop=%d hwnd=%p class='%s'", hops, h, cls_a);
            if (wcscmp(cls, L"ShellTabWindowClass") == 0) {
                active_tab = h;
                break;
            }
            h = ::GetParent(h);
        }
        diag::log("fsb: active_tab (from owner_hwnd walk) = %p", active_tab);
    }
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

                // Primary: ask this IShellBrowser for its active
                // IShellView and check whether the shell view's HWND
                // is a descendant of the right-clicked tab. This is
                // the per-tab discriminator — cabinet HWND alone is
                // shared across tabs and useless here.
                bool tab_match = false;
                HWND hw_sv = nullptr;
                HWND hw_sv_parent = nullptr;
                wchar_t sv_cls[64] = {};
                wchar_t sv_parent_cls[64] = {};
                if (active_tab) {
                    IShellView* sv_probe = nullptr;
                    if (SUCCEEDED(sb->QueryActiveShellView(&sv_probe))
                        && sv_probe) {
                        if (SUCCEEDED(sv_probe->GetWindow(&hw_sv))
                            && hw_sv) {
                            ::GetClassNameW(hw_sv, sv_cls, 64);
                            hw_sv_parent = ::GetParent(hw_sv);
                            if (hw_sv_parent) ::GetClassNameW(hw_sv_parent, sv_parent_cls, 64);
                            if (hw_sv == active_tab ||
                                ::IsChild(active_tab, hw_sv)) {
                                tab_match = true;
                            }
                        }
                        safe_release(sv_probe);
                    }
                }

                bool m = tab_match;
                bool pt_match = false;
                if (!m && SUCCEEDED(ghr) && hw) {
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
                char sv_cls_a[64] = {}; char sv_parent_cls_a[64] = {};
                ::WideCharToMultiByte(CP_UTF8, 0, sv_cls, -1, sv_cls_a, 64, nullptr, nullptr);
                ::WideCharToMultiByte(CP_UTF8, 0, sv_parent_cls, -1, sv_parent_cls_a, 64, nullptr, nullptr);
                diag::log("  fsb iter %ld: GetWindow hr=0x%08x hw=%p "
                          "hw_sv=%p hw_sv_cls='%s' hw_sv_parent=%p hw_sv_parent_cls='%s' "
                          "tab_match=%d "
                          "owner_top=%p owner_rootowner=%p click=(%d,%d) "
                          "match=%d pt_match=%d",
                          i, ghr, hw, hw_sv, sv_cls_a, hw_sv_parent, sv_parent_cls_a, tab_match,
                          owner_top, owner_rootowner,
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

// Walk up from a starting HWND looking for a window whose
// AccessibleObjectFromWindow(OBJID_NATIVEOM, IID_IShellFolderViewDual)
// returns a usable Automation pointer. SHELLDLL_DefView is the usual
// hit; on newer shell builds the shell-view container may have a
// different class name, so we search the whole ancestor chain rather
// than match by class.
static IShellFolderViewDual* find_tab_sfvd(HWND from_hwnd) {
    IShellFolderViewDual* sfvd = nullptr;
    HWND h = from_hwnd;
    for (int hops = 0; h && hops < 16; ++hops) {
        IDispatch* disp = nullptr;
        HRESULT hr = ::AccessibleObjectFromWindow(
            h, (DWORD)OBJID_NATIVEOM,
            IID_IDispatch, (void**)&disp);
        if (SUCCEEDED(hr) && disp) {
            HRESULT qhr = disp->QueryInterface(IID_PPV_ARGS(&sfvd));
            wchar_t cls[64] = {};
            ::GetClassNameW(h, cls, 64);
            char cls_a[64] = {};
            ::WideCharToMultiByte(CP_UTF8, 0, cls, -1,
                                  cls_a, 64, nullptr, nullptr);
            diag::log("  sfvd probe hwnd=%p class='%s' "
                      "AccObjFromWin hr=0x%08x QI hr=0x%08x sfvd=%p",
                      h, cls_a, hr, qhr, (void*)sfvd);
            disp->Release();
            if (SUCCEEDED(qhr) && sfvd) return sfvd;
        }
        h = ::GetParent(h);
    }
    return nullptr;
}

// Path C (qwen 2026-05-23): AccObj the tab HWND DIRECTLY for an
// IServiceProvider, then QueryService(SID_SShellBrowser) for a
// per-tab IShellBrowser. The premise: the tab HWND is the COM site
// for its own IShellBrowser even when the view HWND isn't materialized
// under the tab's subtree (lazy-instantiated tabs).
static IShellBrowser* try_get_per_tab_shell_browser(HWND active_tab) {
    if (!active_tab) return nullptr;

    const DWORD objids[] = { (DWORD)OBJID_NATIVEOM,
                             (DWORD)OBJID_WINDOW };
    for (DWORD objid : objids) {
        IServiceProvider* sp = nullptr;
        HRESULT hr = ::AccessibleObjectFromWindow(
            active_tab, objid,
            IID_IServiceProvider, (void**)&sp);
        diag::log("  tab-sb: AccObj objid=0x%08lx hr=0x%08x sp=%p",
                  objid, hr, (void*)sp);
        if (SUCCEEDED(hr) && sp) {
            IShellBrowser* psb = nullptr;
            HRESULT qhr = sp->QueryService(SID_SShellBrowser,
                                            IID_PPV_ARGS(&psb));
            diag::log("  tab-sb: QueryService(SID_SShellBrowser) "
                      "hr=0x%08x psb=%p", qhr, (void*)psb);
            sp->Release();
            if (SUCCEEDED(qhr) && psb) return psb;
        }
    }
    return nullptr;
}

// Per-tab extraction. Tries qwen's Path C first (per-tab IShellBrowser
// via OBJID_NATIVEOM → IServiceProvider → SID_SShellBrowser on the
// tab HWND itself). Falls back to the SHELLDLL_DefView /
// IShellFolderViewDual probe inside the tab subtree if Path C misses.
//
// Returns true if folder path was extracted (selection may be empty
// for a background click). False → fall back to the legacy
// IShellBrowser path.
static bool try_tab_subtree_extraction(
        HWND active_tab,
        std::string& folder_path_out,
        std::vector<std::string>& sel_paths_out) {
    if (!active_tab) return false;

    // One-shot diagnostic: dump full descendant tree of active_tab AND
    // descendants of its containing CabinetWClass, probing every
    // SHELLDLL_DefView/DirectUIHWND/ShellTabContentClass/etc. with
    // AccObj→IServiceProvider→QueryService for the three candidate SIDs.
    // Runs only once per session to avoid log explosion.
    static bool s_dumped_once = false;
    if (!s_dumped_once) {
        diagnostic_dump_tab(active_tab);
        s_dumped_once = true;
    }

    // ── Path C: per-tab IShellBrowser via the tab HWND's own COM site
    IShellBrowser* psb = try_get_per_tab_shell_browser(active_tab);
    if (psb) {
        IShellView* psv = nullptr;
        if (SUCCEEDED(psb->QueryActiveShellView(&psv)) && psv) {
            IFolderView* fv = nullptr;
            if (SUCCEEDED(psv->QueryInterface(IID_PPV_ARGS(&fv))) && fv) {
                // Folder via IFolderView::GetFolder → IPersistFolder2
                IShellFolder* parent_isf = nullptr;
                if (SUCCEEDED(fv->GetFolder(IID_IShellFolder,
                                            (void**)&parent_isf))
                    && parent_isf) {
                    IPersistFolder2* pf2 = nullptr;
                    if (SUCCEEDED(parent_isf->QueryInterface(
                            IID_PPV_ARGS(&pf2))) && pf2) {
                        LPITEMIDLIST pidl = nullptr;
                        if (SUCCEEDED(pf2->GetCurFolder(&pidl)) && pidl) {
                            folder_path_out = path_from_pidl(pidl);
                            ::CoTaskMemFree(pidl);
                        }
                        safe_release(pf2);
                    }
                    safe_release(parent_isf);
                }
                // Selection via IFolderView::Items(SVGIO_SELECTION)
                IShellItemArray* items = nullptr;
                if (SUCCEEDED(fv->Items(SVGIO_SELECTION,
                                         IID_PPV_ARGS(&items))) && items) {
                    DWORD c = 0;
                    items->GetCount(&c);
                    for (DWORD i = 0; i < c; ++i) {
                        IShellItem* it = nullptr;
                        if (SUCCEEDED(items->GetItemAt(i, &it)) && it) {
                            std::string p = path_from_shell_item(it);
                            diag::log("  path-C sel[%lu] path='%s'",
                                      i, p.c_str());
                            if (!p.empty()) sel_paths_out.push_back(p);
                            safe_release(it);
                        }
                    }
                    safe_release(items);
                }
                safe_release(fv);
            }
            safe_release(psv);
        }
        safe_release(psb);
        if (!folder_path_out.empty()) {
            diag::log("  path-C SUCCESS folder='%s' sel=%zu",
                      folder_path_out.c_str(), sel_paths_out.size());
            return true;
        }
        diag::log("  path-C: psb obtained but extraction empty — "
                  "falling through to SFVD probe");
    }

    // ── Path A (fallback): SHELLDLL_DefView + IShellFolderViewDual.
    // Look for SHELLDLL_DefView (the actual shell-view host class on
    // 24H2 — SysListView32 doesn't exist anywhere in modern Explorer's
    // tab subtree). find_tab_sfvd walks up from there trying
    // OBJID_NATIVEOM + IID_IDispatch + QI IShellFolderViewDual.

    // Locate the tab's listview, then walk up looking for the window
    // that exposes IShellFolderViewDual via OBJID_NATIVEOM. Both have
    // to be inside active_tab's subtree, so the resulting interface
    // is per-tab even when IShellBrowser is shared across tabs.
    HWND def_view = find_descendant_by_class(active_tab, L"SHELLDLL_DefView");
    diag::log("  tab-subtree SHELLDLL_DefView=%p", def_view);

    IShellFolderViewDual* sfvd = def_view ? find_tab_sfvd(def_view) : nullptr;
    if (!sfvd) {
        // Try walking up from active_tab itself as a secondary probe.
        sfvd = find_tab_sfvd(active_tab);
    }
    if (!sfvd) {
        diag::log("  tab-subtree: no IShellFolderViewDual reachable");
        return false;
    }

    // Folder = sfvd->get_Folder() → QI Folder2 → get_Self → get_Path
    Folder* folder = nullptr;
    if (SUCCEEDED(sfvd->get_Folder(&folder)) && folder) {
        Folder2* folder2 = nullptr;
        if (SUCCEEDED(folder->QueryInterface(IID_PPV_ARGS(&folder2)))
            && folder2) {
            FolderItem* self_item = nullptr;
            if (SUCCEEDED(folder2->get_Self(&self_item)) && self_item) {
                BSTR path = nullptr;
                if (SUCCEEDED(self_item->get_Path(&path)) && path) {
                    folder_path_out = narrow_utf8(path);
                    diag::log("  tab-subtree folder='%s'",
                              folder_path_out.c_str());
                    ::SysFreeString(path);
                }
                self_item->Release();
            }
            folder2->Release();
        }
        folder->Release();
    }

    // Selection = sfvd->SelectedItems() → FolderItems → iterate
    FolderItems* items = nullptr;
    if (SUCCEEDED(sfvd->SelectedItems(&items)) && items) {
        long count = 0;
        items->get_Count(&count);
        for (long i = 0; i < count; ++i) {
            VARIANT vi; ::VariantInit(&vi);
            vi.vt = VT_I4; vi.lVal = i;
            FolderItem* it = nullptr;
            if (SUCCEEDED(items->Item(vi, &it)) && it) {
                BSTR path = nullptr;
                if (SUCCEEDED(it->get_Path(&path)) && path) {
                    std::string p = narrow_utf8(path);
                    diag::log("  tab-subtree sel[%ld] path='%s'",
                              i, p.c_str());
                    if (!p.empty()) sel_paths_out.push_back(p);
                    ::SysFreeString(path);
                }
                it->Release();
            }
        }
        items->Release();
    }
    diag::log("  tab-subtree result: sel_count=%zu folder='%s'",
              sel_paths_out.size(), folder_path_out.c_str());

    sfvd->Release();
    return !folder_path_out.empty();
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

    // Per-tab subtree extraction (Tier 1 + Tier 2). This bypasses the
    // shared-IShellBrowser bug on Win11 24H2 tabbed Explorer: walk up
    // from owner_hwnd to find the ShellTabWindowClass HWND of the tab
    // the user right-clicked in, then read selection + folder from
    // that tab's OWN listview subtree rather than via QueryActiveShellView
    // (which gives us whichever tab Explorer considers "active" — often
    // the wrong one).
    HWND active_tab = nullptr;
    if (owner_hwnd) {
        HWND h = owner_hwnd;
        for (int hops = 0; h && hops < 32; ++hops) {
            wchar_t cls[64] = {};
            ::GetClassNameW(h, cls, 64);
            if (wcscmp(cls, L"ShellTabWindowClass") == 0) {
                active_tab = h;
                break;
            }
            h = ::GetParent(h);
        }
    }
    diag::log("populate_shell_context: active_tab=%p", active_tab);

    if (active_tab) {
        std::string tab_folder;
        std::vector<std::string> tab_sel;
        if (try_tab_subtree_extraction(active_tab, tab_folder, tab_sel)) {
            if (tab_sel.empty()) {
                _menu_internal::set_target(ClickTarget::DirectoryBackground);
                if (!tab_folder.empty()) parentFolder = tab_folder;
            } else if (tab_sel.size() == 1) {
                _menu_internal::set_paths(tab_sel);
                _menu_internal::set_target(
                    infer_single_target(tab_sel[0], false));
                if (!tab_folder.empty()) parentFolder = tab_folder;
            } else {
                _menu_internal::set_paths(tab_sel);
                _menu_internal::set_target(ClickTarget::MultiSelection);
                if (!tab_folder.empty()) parentFolder = tab_folder;
            }
            diag::log("populate_shell_context: tab-subtree OK "
                      "target=%d sel=%zu folder=%s",
                      (int)target, tab_sel.size(), tab_folder.c_str());
            if (need_couninit) ::CoUninitialize();
            return true;
        }
        diag::log("populate_shell_context: tab-subtree failed; "
                  "falling back to legacy IShellBrowser path");
    }

    // UIA-direct read: bypass the stale per-cabinet IShellBrowser
    // entirely. Read folder name from the currently-selected UIA
    // TabItem (its Name = full folder path) and selection from the
    // per-tab SHELLDLL_DefView's SelectionPattern. This is the only
    // path that returns the RIGHT tab's data on Win11 24H2 because
    // IShellWindows-registered IShellBrowser is anchored permanently
    // to the cabinet's original tab and never refreshes on tab
    // switch. Lossy on hidden extensions / virtual items.
    if (active_tab) {
        std::string uia_folder;
        std::vector<std::string> uia_sel;
        if (read_active_tab_via_uia(active_tab, uia_folder, uia_sel)) {
            if (uia_sel.empty()) {
                _menu_internal::set_target(ClickTarget::DirectoryBackground);
            } else if (uia_sel.size() == 1) {
                _menu_internal::set_paths(uia_sel);
                _menu_internal::set_target(
                    infer_single_target(uia_sel[0], false));
            } else {
                _menu_internal::set_paths(uia_sel);
                _menu_internal::set_target(ClickTarget::MultiSelection);
            }
            if (!uia_folder.empty()) parentFolder = uia_folder;
            diag::log("populate_shell_context: UIA-read OK target=%d "
                      "sel=%zu folder=%s",
                      (int)target, uia_sel.size(), uia_folder.c_str());
            if (need_couninit) ::CoUninitialize();
            return true;
        }
        diag::log("populate_shell_context: UIA-read failed; falling "
                  "back to legacy IShellBrowser path");
    }

    bool is_desktop = false;
    IShellBrowser* sb = find_shell_browser_for(owner_hwnd, owner_top,
                                               owner_rootowner,
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

                // Fallback: if SVGIO_SELECTION returned zero items but
                // the click landed on an actual item, hit-test the
                // click point against every item's bounding rect.
                // This handles the Win11 24H2 bug where
                // IFolderView::Items(SVGIO_SELECTION) returns 0 even
                // when a file/folder is selected.
                //
                // Strategy: ask IShellView for the underlying list-view
                // window (SVGIO_BACKGROUND), then use LVM_GETITEMRECT
                // to get each item's client-area rect.  We deliberately
                // do NOT use IFolderView2::GetFocusedItem — it returns
                // the *persistent* selection which mis-classifies
                // background clicks.
                if (sel_paths.empty() && !folder_path.empty()) {
                    int total = 0;
                    if (SUCCEEDED(fv->ItemCount(SVGIO_ALLVIEW,
                                                &total))) {
                        // Get the list-view HWND from the shell view.
                        HWND hwndLV = nullptr;
                        if (SUCCEEDED(sv->GetItemObject(
                                SVGIO_BACKGROUND, IID_IUnknown,
                                (void**)&hwndLV))
                            && hwndLV) {
                            POINT click_pt = { screen_x, screen_y };
                            ::ScreenToClient(hwndLV, &click_pt);
                            for (int i = 0; i < total && sel_paths.empty();
                                 ++i) {
                                RECT rc = {};
                                if (::SendMessageW(hwndLV,
                                        LVM_GETITEMRECT,
                                        (WPARAM)i,
                                        (LPARAM)&rc)) {
                                    if (::PtInRect(&rc, click_pt)) {
                                        // Click hit item i — resolve
                                        // PIDL to path.
                                        LPITEMIDLIST pidl = nullptr;
                                        if (SUCCEEDED(fv->Item(i,
                                                &pidl)) && pidl) {
                                            std::string p =
                                                path_from_pidl(pidl);
                                            diag::log("  hit-test[%d] "
                                                      "path='%s'", i,
                                                      p.c_str());
                                            if (!p.empty())
                                                sel_paths.push_back(p);
                                            ::CoTaskMemFree(pidl);
                                        }
                                    }
                                }
                            }
                        }
                    }
                    diag::log("  hit-test fallback: sel_paths=%zu",
                              sel_paths.size());
                }

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
