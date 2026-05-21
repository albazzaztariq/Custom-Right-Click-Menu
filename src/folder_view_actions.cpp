// folder_view_actions.cpp
// Drive the active File Explorer view via IFolderView2.
//
// We can't reuse populate_shell_context's owner-HWND-driven lookup
// because by the time a Selection handler fires, our menu window has
// been destroyed and the click HWND is gone. Instead we walk
// IShellWindows looking for the foreground explorer window — the
// window the user was right-clicking in is, by definition, the one
// that just had focus before our menu briefly stole it.

#include "folder_view_actions.h"
#include "menu_api.h"
#include "diag.h"

#include <windows.h>
#include <shlobj.h>
#include <shobjidl.h>
#include <exdisp.h>

#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "oleaut32.lib")

// PROPERTYKEY values from propkey.h. Declared here as literals so we
// don't have to deal with INITGUID / propsys.lib, both of which collide
// with shlobj.h's DEFINE_SHLGUID emissions in this TU.
//
// Format: { FMTID guid, PID }. The first three columns are FMTID,
// last is PID. Values come from propkey.h verbatim.
static const PROPERTYKEY kPKEY_ItemNameDisplay = {
    { 0xB725F130, 0x47EF, 0x101A,
      { 0xA5,0xF1,0x02,0x60,0x8C,0x9E,0xEB,0xAC } }, 10 };
static const PROPERTYKEY kPKEY_DateModified   = {
    { 0xB725F130, 0x47EF, 0x101A,
      { 0xA5,0xF1,0x02,0x60,0x8C,0x9E,0xEB,0xAC } }, 14 };
static const PROPERTYKEY kPKEY_Size           = {
    { 0xB725F130, 0x47EF, 0x101A,
      { 0xA5,0xF1,0x02,0x60,0x8C,0x9E,0xEB,0xAC } }, 12 };
static const PROPERTYKEY kPKEY_ItemTypeText   = {
    { 0x28636AA6, 0x953D, 0x11D2,
      { 0xB5,0xD6,0x00,0xC0,0x4F,0xD9,0x18,0xD0 } }, 11 };

namespace {

template <class T>
void safe_release(T*& p) { if (p) { p->Release(); p = nullptr; } }

// Walk IShellWindows; return IFolderView2* for the window that is
// currently in the foreground, or the first explorer window if none
// match. Caller releases.
IFolderView2* find_active_folder_view2() {
    IShellWindows* psw = nullptr;
    HRESULT hr = ::CoCreateInstance(CLSID_ShellWindows, nullptr,
                                    CLSCTX_ALL, IID_PPV_ARGS(&psw));
    if (FAILED(hr) || !psw) {
        diag::log("folder_view_actions: CoCreateInstance(ShellWindows) "
                  "hr=0x%08x", hr);
        return nullptr;
    }

    HWND fg = ::GetForegroundWindow();
    if (fg) fg = ::GetAncestor(fg, GA_ROOT);

    long count = 0;
    psw->get_Count(&count);

    IFolderView2* match    = nullptr;
    IFolderView2* fallback = nullptr;

    for (long i = 0; i < count; ++i) {
        VARIANT vi; ::VariantInit(&vi);
        vi.vt = VT_I4; vi.lVal = i;
        IDispatch* disp = nullptr;
        if (FAILED(psw->Item(vi, &disp)) || !disp) continue;

        IServiceProvider* sp = nullptr;
        IShellBrowser*    sb = nullptr;
        IShellView*       sv = nullptr;
        IFolderView2*     fv = nullptr;

        if (SUCCEEDED(disp->QueryInterface(IID_PPV_ARGS(&sp))) && sp) {
            if (SUCCEEDED(sp->QueryService(SID_STopLevelBrowser,
                                           IID_PPV_ARGS(&sb))) && sb) {
                HWND hw = nullptr;
                sb->GetWindow(&hw);
                if (SUCCEEDED(sb->QueryActiveShellView(&sv)) && sv) {
                    if (SUCCEEDED(sv->QueryInterface(
                            IID_PPV_ARGS(&fv))) && fv) {
                        if (hw == fg && !match) {
                            match = fv;
                            fv = nullptr;       // transfer ref
                        } else if (!fallback) {
                            fallback = fv;
                            fv = nullptr;
                        }
                    }
                }
            }
        }
        safe_release(fv);
        safe_release(sv);
        safe_release(sb);
        safe_release(sp);
        safe_release(disp);

        if (match) break;
    }

    safe_release(psw);

    if (match) {
        safe_release(fallback);
        return match;
    }
    return fallback;
}

bool view_mode_to_fvm(_folder_view_actions::ViewMode m,
                      FOLDERVIEWMODE& out_mode, int& out_size) {
    using V = _folder_view_actions::ViewMode;
    // Win11 24H2 collapses FVM_SMALLICON onto medium-icon rendering;
    // to actually get the Small-icons size we stay on FVM_ICON and
    // shrink the icon size. The literal values 96/48/16 match the
    // sizes the native Explorer "View" menu picks for its
    // Large / Medium / Small icon modes.
    switch (m) {
        case V::LargeIcons:  out_mode = FVM_ICON;    out_size = 96; return true;
        case V::MediumIcons: out_mode = FVM_ICON;    out_size = 48; return true;
        case V::SmallIcons:  out_mode = FVM_ICON;    out_size = 16; return true;
        case V::List:        out_mode = FVM_LIST;    out_size = -1; return true;
        case V::Details:     out_mode = FVM_DETAILS; out_size = -1; return true;
    }
    return false;
}

const PROPERTYKEY* sort_key_to_pkey(_folder_view_actions::SortKey k) {
    using S = _folder_view_actions::SortKey;
    switch (k) {
        case S::Name:         return &kPKEY_ItemNameDisplay;
        case S::DateModified: return &kPKEY_DateModified;
        case S::Size:         return &kPKEY_Size;
        case S::Type:         return &kPKEY_ItemTypeText;
    }
    return nullptr;
}

const PROPERTYKEY* group_key_to_pkey(_folder_view_actions::GroupKey k) {
    using G = _folder_view_actions::GroupKey;
    switch (k) {
        case G::Name:         return &kPKEY_ItemNameDisplay;
        case G::DateModified: return &kPKEY_DateModified;
        case G::Type:         return &kPKEY_ItemTypeText;
        case G::None:         return nullptr;
    }
    return nullptr;
}

}  // namespace


namespace _folder_view_actions {

bool apply_view_mode(ViewMode m) {
    bool ok = false;
    __try {
        FOLDERVIEWMODE fvm; int size;
        if (!view_mode_to_fvm(m, fvm, size)) return false;

        IFolderView2* fv = find_active_folder_view2();
        if (!fv) {
            diag::log("apply_view_mode: no active folder view");
            return false;
        }
        HRESULT hr = fv->SetViewModeAndIconSize(fvm, size);
        diag::log("apply_view_mode: mode=%d size=%d hr=0x%08x",
                  (int)fvm, size, hr);
        ok = SUCCEEDED(hr);
        safe_release(fv);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        diag::log("apply_view_mode: SEH fault");
        ok = false;
    }
    return ok;
}

bool apply_sort_by(SortKey k) {
    bool ok = false;
    __try {
        const PROPERTYKEY* pk = sort_key_to_pkey(k);
        if (!pk) return false;

        IFolderView2* fv = find_active_folder_view2();
        if (!fv) {
            diag::log("apply_sort_by: no active folder view");
            return false;
        }
        SORTCOLUMN col = {};
        col.propkey   = *pk;
        col.direction = SORT_ASCENDING;
        HRESULT hr = fv->SetSortColumns(&col, 1);
        diag::log("apply_sort_by: pkey={%lx-...,%d} hr=0x%08x",
                  pk->fmtid.Data1, pk->pid, hr);
        ok = SUCCEEDED(hr);
        safe_release(fv);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        diag::log("apply_sort_by: SEH fault");
        ok = false;
    }
    return ok;
}

bool apply_group_by(GroupKey k) {
    bool ok = false;
    __try {
        IFolderView2* fv = find_active_folder_view2();
        if (!fv) {
            diag::log("apply_group_by: no active folder view");
            return false;
        }

        HRESULT hr;
        const PROPERTYKEY* pk = group_key_to_pkey(k);
        if (pk) {
            hr = fv->SetGroupBy(*pk, TRUE);
        } else {
            // None — pass an empty PROPERTYKEY to ungroup.
            PROPERTYKEY empty = {};
            hr = fv->SetGroupBy(empty, TRUE);
        }
        diag::log("apply_group_by: key=%d hr=0x%08x", (int)k, hr);
        ok = SUCCEEDED(hr);
        safe_release(fv);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        diag::log("apply_group_by: SEH fault");
        ok = false;
    }
    return ok;
}

}  // namespace _folder_view_actions


// ─────────────────────────────────────────────────────────────────────
// Public API forwarders (declared in menu_api.h).
// Map the consumer-facing enums to the internal enums and dispatch.
// ─────────────────────────────────────────────────────────────────────

bool set_view_mode(ViewMode m) {
    using F = _folder_view_actions::ViewMode;
    F fm;
    switch (m) {
        case ViewMode::LargeIcons:  fm = F::LargeIcons;  break;
        case ViewMode::MediumIcons: fm = F::MediumIcons; break;
        case ViewMode::SmallIcons:  fm = F::SmallIcons;  break;
        case ViewMode::List:        fm = F::List;        break;
        case ViewMode::Details:     fm = F::Details;     break;
        default: return false;
    }
    return _folder_view_actions::apply_view_mode(fm);
}

bool set_sort_by(SortKey k) {
    using F = _folder_view_actions::SortKey;
    F fk;
    switch (k) {
        case SortKey::Name:         fk = F::Name;         break;
        case SortKey::DateModified: fk = F::DateModified; break;
        case SortKey::Size:         fk = F::Size;         break;
        case SortKey::Type:         fk = F::Type;         break;
        default: return false;
    }
    return _folder_view_actions::apply_sort_by(fk);
}

bool set_group_by(GroupKey k) {
    using F = _folder_view_actions::GroupKey;
    F fk;
    switch (k) {
        case GroupKey::Name:         fk = F::Name;         break;
        case GroupKey::DateModified: fk = F::DateModified; break;
        case GroupKey::Type:         fk = F::Type;         break;
        case GroupKey::None:         fk = F::None;         break;
        default: return false;
    }
    return _folder_view_actions::apply_group_by(fk);
}
