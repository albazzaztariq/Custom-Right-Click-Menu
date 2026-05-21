// hook.cpp
// IAT hook implementation. Walks PE structures by hand — no MinHook or
// Detours dependency for the IAT patching itself.
//
// Reference: Nilesoft Shell's src/dll/src/Include/Hooker.h (same
// algorithm, simplified).

#include "hook.h"
#include "menu_node.h"
#include "menu_window.h"
#include "context.h"
#include "menu_api.h"
#include "shell_context.h"
#include "diag.h"
#include <psapi.h>
#include <vector>
#include <string>
#include <cwctype>

#pragma comment(lib, "psapi.lib")

// Defined in main.cpp. Resets per-click state and calls the consumer's
// RegisterMenu() under SEH.
extern "C" void build_menu_for_current_click();


// ─────────────────────────────────────────────────────────────────────
// IATHook implementation
// ─────────────────────────────────────────────────────────────────────

IATHook::~IATHook() {
    if (_installed) uninstall();
}

IATHook& IATHook::init(HMODULE target_module,
                       const wchar_t* import_dll,
                       const char* import_func,
                       void* detour) {
    _target = target_module;
    _dll    = import_dll;
    _func   = import_func;
    _detour = detour;
    return *this;
}

static bool ieq_wcs(const wchar_t* a, const char* b) {
    while (*a && *b) {
        wchar_t ca = towlower(*a);
        wchar_t cb = towlower(static_cast<unsigned char>(*b));
        if (ca != cb) return false;
        ++a; ++b;
    }
    return *a == 0 && *b == 0;
}

bool IATHook::install() {
    if (_installed) return true;
    if (!_target)   return false;

    auto base = reinterpret_cast<BYTE*>(_target);
    auto dos  = reinterpret_cast<IMAGE_DOS_HEADER*>(base);
    if (dos->e_magic != IMAGE_DOS_SIGNATURE) return false;

    auto nt = reinterpret_cast<IMAGE_NT_HEADERS*>(base + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE) return false;

    auto& dir = nt->OptionalHeader
                  .DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    if (dir.VirtualAddress == 0 || dir.Size == 0) return false;

    auto imp = reinterpret_cast<IMAGE_IMPORT_DESCRIPTOR*>(
                  base + dir.VirtualAddress);

    for (; imp->Name; ++imp) {
        const char* dll_name =
            reinterpret_cast<const char*>(base + imp->Name);
        if (!ieq_wcs(_dll, dll_name)) continue;

        diag::log("    import descriptor: dll=%s INT_rva=0x%x "
                  "IAT_rva=0x%x", dll_name,
                  imp->OriginalFirstThunk, imp->FirstThunk);

        // Some imports lack an INT (OriginalFirstThunk == 0). For
        // those, the IAT is also the name table at load time (the
        // loader rewrites it in place). Use FirstThunk as the source
        // of names when INT is absent.
        DWORD names_rva = imp->OriginalFirstThunk
                          ? imp->OriginalFirstThunk
                          : imp->FirstThunk;
        auto orig = reinterpret_cast<IMAGE_THUNK_DATA*>(
                      base + names_rva);
        auto iat  = reinterpret_cast<IMAGE_THUNK_DATA*>(
                      base + imp->FirstThunk);

        for (; orig->u1.AddressOfData; ++orig, ++iat) {
            if (IMAGE_SNAP_BY_ORDINAL(orig->u1.Ordinal)) continue;

            auto by_name = reinterpret_cast<IMAGE_IMPORT_BY_NAME*>(
                              base + orig->u1.AddressOfData);
            if (lstrcmpA(by_name->Name, _func) != 0) continue;

            void** slot = reinterpret_cast<void**>(&iat->u1.Function);
            DWORD oldProtect = 0;
            if (!::VirtualProtect(slot, sizeof(void*),
                                  PAGE_READWRITE, &oldProtect)) {
                diag::log("    VirtualProtect failed for slot %p, "
                          "GLE=%lu", slot, ::GetLastError());
                return false;
            }

            _slot       = slot;
            _orig       = *slot;
            *slot       = _detour;
            _installed  = true;

            DWORD ignored = 0;
            ::VirtualProtect(slot, sizeof(void*), oldProtect, &ignored);

            diag::log("    slot=%p orig=%p new=%p readback=%p (%s)",
                      slot, _orig, _detour, *slot, _func);
            return true;
        }
    }
    return false;
}

bool IATHook::uninstall() {
    if (!_installed || !_slot) return false;

    DWORD oldProtect = 0;
    if (!::VirtualProtect(_slot, sizeof(void*),
                          PAGE_READWRITE, &oldProtect)) {
        return false;
    }
    *_slot = _orig;
    DWORD ignored = 0;
    ::VirtualProtect(_slot, sizeof(void*), oldProtect, &ignored);
    _installed = false;
    return true;
}


// ─────────────────────────────────────────────────────────────────────
// Detour implementations.
//
// When explorer (or any module in this process) calls TrackPopupMenu*,
// these run instead. Sequence:
//   1) Populate context globals. (Currently hard-coded to
//      DirectoryBackground — real shell-data extraction is a follow-up
//      task.)
//   2) Call build_menu_for_current_click() to run the consumer's
//      RegisterMenu() under SEH.
//   3) If the consumer produced no entries, fall back to the original
//      TrackPopupMenu so explorer's menu still shows.
//   4) Otherwise show our owner-drawn window at the requested coords.
//   5) On click, dispatch the handler.
//   6) Wrapped in SEH — any fault inside our code falls back to
//      calling the original, never crashes explorer.
// ─────────────────────────────────────────────────────────────────────

using TrackPopupMenu_t   = BOOL (WINAPI*)(HMENU, UINT, int, int, int,
                                          HWND, const RECT*);
using TrackPopupMenuEx_t = BOOL (WINAPI*)(HMENU, UINT, int, int,
                                          HWND, LPTPMPARAMS);

static TrackPopupMenu_t    g_orig_TrackPopupMenu    = nullptr;
static TrackPopupMenuEx_t  g_orig_TrackPopupMenuEx  = nullptr;

static void prime_context(HWND owner, int screen_x, int screen_y) {
    // populate_shell_context resets context, populates modifiers and
    // click location, and tries to extract real selection from
    // IShellWindows. If extraction fails (e.g. caller isn't an explorer
    // window — hook_test, or a non-shell host), we fall back to the
    // safe default of DirectoryBackground so the menu still renders.
    bool ok = false;
    __try {
        ok = populate_shell_context(owner, screen_x, screen_y);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        ok = false;
    }
    if (!ok) {
        _menu_internal::reset_context();
        _menu_internal::set_click_location(screen_x, screen_y);
        _menu_internal::set_target(ClickTarget::DirectoryBackground);

        bool shift = (GetKeyState(VK_SHIFT)   & 0x8000) != 0;
        bool ctrl  = (GetKeyState(VK_CONTROL) & 0x8000) != 0;
        bool alt   = (GetKeyState(VK_MENU)    & 0x8000) != 0;
        bool win   = (GetKeyState(VK_LWIN)    & 0x8000) != 0
                  || (GetKeyState(VK_RWIN)    & 0x8000) != 0;
        _menu_internal::set_modifiers(shift, ctrl, alt, win);
    }
}

static int show_our_menu(int x, int y, HWND owner) {
    if (_menu_internal::rootMenu.children.empty()) return -1;
    return menu_window::show(_menu_internal::rootMenu, x, y, owner);
}

static void dispatch_handler(int id) {
    if (id < 0) return;
    if (id >= (int)_menu_internal::handlerRegistry.size()) return;
    __try {
        _menu_internal::handlerRegistry[id]();
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        // Consumer handler faulted — swallow.
    }
}

// Inline-hook bookkeeping. Full definition + patch/unpatch lives
// further down; the struct + globals are forward-defined here so
// with_inline_hook_lifted (used by the detours below) can see them.
struct InlineHook {
    void* target  = nullptr;
    BYTE  saved[16] = {};
    bool  installed = false;
};
static InlineHook g_inline_TPMEX;
static InlineHook g_inline_TPM;

// Run `body` (a callable returning BOOL) with the named inline hook
// temporarily lifted, so a direct call to the original function entry
// doesn't recurse back into our detour. Repatches in all exit paths.
template <class F>
static BOOL with_inline_hook_lifted(InlineHook& h, F&& body) {
    void* target = h.target;
    void* detour = nullptr;
    // Recover the detour from the patch we wrote (mov rax, imm64).
    // h.saved holds the original bytes; the live bytes hold the patch.
    BYTE live[12]; memcpy(live, target, 12);
    memcpy(&detour, &live[2], 8);

    // Lift: restore original bytes.
    DWORD oldp = 0;
    if (::VirtualProtect(target, 12, PAGE_EXECUTE_READWRITE, &oldp)) {
        memcpy(target, h.saved, 12);
        DWORD ign = 0; ::VirtualProtect(target, 12, oldp, &ign);
        ::FlushInstructionCache(::GetCurrentProcess(), target, 12);
    }

    BOOL result = FALSE;
    __try { result = body(); } __except (EXCEPTION_EXECUTE_HANDLER) {}

    // Repatch.
    if (::VirtualProtect(target, 12, PAGE_EXECUTE_READWRITE, &oldp)) {
        BYTE patch[12];
        patch[0] = 0x48; patch[1] = 0xB8;
        memcpy(&patch[2], &detour, 8);
        patch[10] = 0xFF; patch[11] = 0xE0;
        memcpy(target, patch, 12);
        DWORD ign = 0; ::VirtualProtect(target, 12, oldp, &ign);
        ::FlushInstructionCache(::GetCurrentProcess(), target, 12);
    }
    return result;
}

static BOOL WINAPI TrackPopupMenu_detour(
        HMENU hMenu, UINT uFlags, int x, int y, int nReserved,
        HWND hWnd, const RECT* prcRect) {
    // Shift+RightClick: bypass our menu, show the legacy system menu.
    if ((::GetAsyncKeyState(VK_SHIFT) & 0x8000) != 0
        && g_inline_TPM.installed && g_orig_TrackPopupMenu) {
        diag::log("TrackPopupMenu_detour: Shift held -> bypass to legacy");
        return with_inline_hook_lifted(g_inline_TPM, [&]() {
            return g_orig_TrackPopupMenu(hMenu, uFlags, x, y,
                                         nReserved, hWnd, prcRect);
        });
    }
    __try {
        prime_context(hWnd, x, y);
        build_menu_for_current_click();

        if (_menu_internal::rootMenu.children.empty()) {
            // Consumer didn't define a menu for this target — fall
            // through to the legacy menu. Lift our inline patch so the
            // direct call to g_orig_TrackPopupMenu doesn't recurse.
            if (g_inline_TPM.installed && g_orig_TrackPopupMenu) {
                return with_inline_hook_lifted(g_inline_TPM, [&]() {
                    return g_orig_TrackPopupMenu(hMenu, uFlags, x, y,
                                                 nReserved, hWnd, prcRect);
                });
            }
            return FALSE;
        }

        int clicked = show_our_menu(x, y, hWnd);
        dispatch_handler(clicked);
        return TRUE;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        if (g_inline_TPM.installed && g_orig_TrackPopupMenu) {
            return with_inline_hook_lifted(g_inline_TPM, [&]() {
                return g_orig_TrackPopupMenu(hMenu, uFlags, x, y,
                                             nReserved, hWnd, prcRect);
            });
        }
        return FALSE;
    }
}

static BOOL WINAPI TrackPopupMenuEx_detour(
        HMENU hMenu, UINT uFlags, int x, int y,
        HWND hWnd, LPTPMPARAMS lptpm) {
    diag::log("TrackPopupMenuEx_detour FIRED at (%d,%d) hMenu=%p",
              x, y, hMenu);
    // Shift+RightClick: bypass our menu, show the legacy system menu.
    if ((::GetAsyncKeyState(VK_SHIFT) & 0x8000) != 0
        && g_inline_TPMEX.installed && g_orig_TrackPopupMenuEx) {
        diag::log("  Shift held -> bypass to legacy TrackPopupMenuEx");
        return with_inline_hook_lifted(g_inline_TPMEX, [&]() {
            return g_orig_TrackPopupMenuEx(hMenu, uFlags, x, y,
                                           hWnd, lptpm);
        });
    }
    (void)hMenu; (void)uFlags; (void)lptpm;
    __try {
        prime_context(hWnd, x, y);
        build_menu_for_current_click();

        if (_menu_internal::rootMenu.children.empty()) {
            diag::log("  no menu items -> fall through to legacy TPMEx");
            if (g_inline_TPMEX.installed && g_orig_TrackPopupMenuEx) {
                return with_inline_hook_lifted(g_inline_TPMEX, [&]() {
                    return g_orig_TrackPopupMenuEx(hMenu, uFlags, x, y,
                                                   hWnd, lptpm);
                });
            }
            return FALSE;
        }

        int clicked = show_our_menu(x, y, hWnd);
        diag::log("  show_our_menu returned %d", clicked);
        dispatch_handler(clicked);
        return TRUE;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        diag::log("  SEH caught in detour");
        if (g_inline_TPMEX.installed && g_orig_TrackPopupMenuEx) {
            return with_inline_hook_lifted(g_inline_TPMEX, [&]() {
                return g_orig_TrackPopupMenuEx(hMenu, uFlags, x, y,
                                               hWnd, lptpm);
            });
        }
        return FALSE;
    }
}


// ─────────────────────────────────────────────────────────────────────
// Bulk install / uninstall
// ─────────────────────────────────────────────────────────────────────

namespace {
    std::vector<IATHook> g_hooks;

    std::vector<HMODULE> enumerate_loaded_modules() {
        std::vector<HMODULE> result;
        HANDLE proc = ::GetCurrentProcess();
        DWORD  needed = 0;
        if (!::EnumProcessModules(proc, nullptr, 0, &needed)) {
            return result;
        }
        result.resize(needed / sizeof(HMODULE));
        if (!::EnumProcessModules(proc, result.data(),
                                  static_cast<DWORD>(result.size() *
                                                     sizeof(HMODULE)),
                                  &needed)) {
            return {};
        }
        return result;
    }
}

static void* g_last_patched_slot = nullptr;

extern "C" __declspec(dllexport) void* get_patched_slot_address() {
    return g_last_patched_slot;
}

// ─────────────────────────────────────────────────────────────────────
// Inline hook — patch the function entry itself.
//
// IAT hooking turned out to be unreliable in this codebase (the
// patched slot doesn't seem to be the one the call site uses, despite
// matching addresses). Inline hooking sidesteps the whole question:
// we overwrite the first 12 bytes of user32!TrackPopupMenuEx with
// `mov rax, detour; jmp rax`. Any call that reaches the function
// entry — regardless of which module's IAT it came through, or
// whether it was found by GetProcAddress — lands in our detour.
//
// Tradeoff: we can't easily call the original anymore (its first
// instructions are gone). For the MVP we don't need to — if our menu
// has no items we return FALSE and the caller sees a no-op. Proper
// "original" support would require a trampoline that saves the
// overwritten bytes and jumps back to function+12.
// ─────────────────────────────────────────────────────────────────────

// InlineHook struct + g_inline_TPM/TPMEX declared earlier (above the
// detours). inline_patch/inline_unpatch follow.

static bool inline_patch(InlineHook& h, void* target, void* detour) {
    h.target = target;
    BYTE patch[12];
    patch[0] = 0x48;                 // mov rax, imm64
    patch[1] = 0xB8;
    memcpy(&patch[2], &detour, 8);
    patch[10] = 0xFF;                // jmp rax
    patch[11] = 0xE0;

    DWORD oldProtect = 0;
    if (!::VirtualProtect(target, 12, PAGE_EXECUTE_READWRITE,
                          &oldProtect)) {
        diag::log("inline_patch: VirtualProtect failed GLE=%lu",
                  ::GetLastError());
        return false;
    }
    memcpy(h.saved, target, 12);
    memcpy(target, patch, 12);
    DWORD ignored = 0;
    ::VirtualProtect(target, 12, oldProtect, &ignored);
    ::FlushInstructionCache(::GetCurrentProcess(), target, 12);
    h.installed = true;

    diag::log("inline_patch: patched %p, saved=[%02x %02x %02x %02x ...] "
              "new detour=%p",
              target, h.saved[0], h.saved[1], h.saved[2], h.saved[3],
              detour);
    return true;
}

static void inline_unpatch(InlineHook& h) {
    if (!h.installed) return;
    DWORD oldProtect = 0;
    if (::VirtualProtect(h.target, 12, PAGE_EXECUTE_READWRITE,
                         &oldProtect)) {
        memcpy(h.target, h.saved, 12);
        DWORD ignored = 0;
        ::VirtualProtect(h.target, 12, oldProtect, &ignored);
        ::FlushInstructionCache(::GetCurrentProcess(), h.target, 12);
    }
    h.installed = false;
}

// Install TPM hooks on a single module. Patches the IAT entries for
// user32!TrackPopupMenu(Ex) AND win32u!NtUserTrackPopupMenuEx — the
// last one is the kernel-syscall thunk that some modern Win11 menu
// paths call directly, bypassing the user32 wrappers.
static int install_tpm_hooks_on_module(HMODULE m) {
    wchar_t mname[MAX_PATH] = {};
    ::GetModuleFileNameW(m, mname, MAX_PATH);

    int n = 0;
    IATHook h1, h2;
    h1.init(m, L"user32.dll", "TrackPopupMenu",
            reinterpret_cast<void*>(&TrackPopupMenu_detour));
    if (h1.install()) {
        g_hooks.push_back(std::move(h1));
        diag::log("  patched %ls : TrackPopupMenu", mname);
        ++n;
    }

    h2.init(m, L"user32.dll", "TrackPopupMenuEx",
            reinterpret_cast<void*>(&TrackPopupMenuEx_detour));
    if (h2.install()) {
        g_last_patched_slot = h2.slot_address();
        g_hooks.push_back(std::move(h2));
        diag::log("  patched %ls : TrackPopupMenuEx", mname);
        ++n;
    }
    return n;
}

void install_iat_hooks() {
    HMODULE user32 = ::GetModuleHandleW(L"user32.dll");
    if (user32) {
        g_orig_TrackPopupMenu   = reinterpret_cast<TrackPopupMenu_t>(
            ::GetProcAddress(user32, "TrackPopupMenu"));
        g_orig_TrackPopupMenuEx = reinterpret_cast<TrackPopupMenuEx_t>(
            ::GetProcAddress(user32, "TrackPopupMenuEx"));
        diag::log("install_iat_hooks: orig TrackPopupMenu=%p "
                  "orig TrackPopupMenuEx=%p",
                  g_orig_TrackPopupMenu, g_orig_TrackPopupMenuEx);
    } else {
        diag::log("install_iat_hooks: user32.dll not loaded?!");
    }

    auto modules = enumerate_loaded_modules();
    diag::log("install_iat_hooks: walking %zu modules", modules.size());

    int n_installed = 0;
    for (HMODULE m : modules) {
        n_installed += install_tpm_hooks_on_module(m);
    }
    diag::log("install_iat_hooks: %d IAT hooks installed", n_installed);

    // Inline hooks on the function entries themselves — catches every
    // caller regardless of which IAT they came through, including
    // direct GetProcAddress + indirect-call patterns.
    if (g_orig_TrackPopupMenuEx) {
        inline_patch(g_inline_TPMEX,
                     reinterpret_cast<void*>(g_orig_TrackPopupMenuEx),
                     reinterpret_cast<void*>(&TrackPopupMenuEx_detour));
    }

    // Also patch user32!TrackPopupMenu (non-Ex). Some older code paths
    // in explorer still use this entry. Signature differs from Ex (it
    // takes nReserved + RECT*), so we use TrackPopupMenu_detour.
    if (g_orig_TrackPopupMenu) {
        inline_patch(g_inline_TPM,
                     reinterpret_cast<void*>(g_orig_TrackPopupMenu),
                     reinterpret_cast<void*>(&TrackPopupMenu_detour));
    }
}

void uninstall_iat_hooks() {
    inline_unpatch(g_inline_TPMEX);
    inline_unpatch(g_inline_TPM);
    for (auto& h : g_hooks) h.uninstall();
    g_hooks.clear();
}


// ─────────────────────────────────────────────────────────────────────
// LoadLibrary hook — defense-in-depth.
//
// Modules loaded AFTER our DllMain finishes won't have had their IATs
// patched. The inline hook on user32!TrackPopupMenuEx already catches
// every caller (regardless of IAT), so this is belt-and-suspenders.
// Mechanism: IAT-hook kernel32!LoadLibraryExW everywhere. In the detour
// we call through to the real implementation, then walk the freshly-
// loaded HMODULE and patch its TrackPopupMenu* imports.
//
// We don't hook LoadLibraryA / LoadLibraryW / LoadLibraryExA — they all
// funnel through LoadLibraryExW inside kernel32, so one hook covers
// the whole family.
// ─────────────────────────────────────────────────────────────────────

using LoadLibraryExW_t = HMODULE (WINAPI*)(LPCWSTR, HANDLE, DWORD);
static LoadLibraryExW_t g_orig_LoadLibraryExW = nullptr;
static std::vector<IATHook> g_ll_hooks;

static HMODULE WINAPI LoadLibraryExW_detour(LPCWSTR name, HANDLE file,
                                            DWORD flags) {
    if (!g_orig_LoadLibraryExW) return nullptr;
    HMODULE m = g_orig_LoadLibraryExW(name, file, flags);
    if (!m) return m;

    // Skip the data-only / resource-only loads — they have no executable
    // imports to patch and the PE walker may not even like them.
    const DWORD kDataOnly = LOAD_LIBRARY_AS_DATAFILE
                          | LOAD_LIBRARY_AS_DATAFILE_EXCLUSIVE
                          | LOAD_LIBRARY_AS_IMAGE_RESOURCE;
    if (flags & kDataOnly) return m;

    __try {
        install_tpm_hooks_on_module(m);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        // Anything weird in the new module's PE: skip it. The inline
        // hook still catches callers via the function entry.
    }
    return m;
}

void install_loadlibrary_hook() {
    HMODULE k32 = ::GetModuleHandleW(L"kernel32.dll");
    if (!k32) {
        diag::log("install_loadlibrary_hook: kernel32.dll not loaded?!");
        return;
    }
    g_orig_LoadLibraryExW = reinterpret_cast<LoadLibraryExW_t>(
        ::GetProcAddress(k32, "LoadLibraryExW"));
    if (!g_orig_LoadLibraryExW) {
        diag::log("install_loadlibrary_hook: no LoadLibraryExW export?!");
        return;
    }

    auto modules = enumerate_loaded_modules();
    int n = 0;
    for (HMODULE m : modules) {
        IATHook h;
        h.init(m, L"kernel32.dll", "LoadLibraryExW",
               reinterpret_cast<void*>(&LoadLibraryExW_detour));
        if (h.install()) {
            g_ll_hooks.push_back(std::move(h));
            ++n;
        }
    }
    diag::log("install_loadlibrary_hook: %d IAT hooks on LoadLibraryExW", n);
}

void uninstall_loadlibrary_hook() {
    for (auto& h : g_ll_hooks) h.uninstall();
    g_ll_hooks.clear();
    g_orig_LoadLibraryExW = nullptr;
}
