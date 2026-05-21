// hook.h
// IAT (Import Address Table) hooking and the public install/uninstall
// entry points used by main.cpp.
//
// The hooking primitive is implemented in hook.cpp following the
// approach in Nilesoft Shell's Hooker.h: walk the PE headers of a
// target module, find the import descriptor for the target DLL, locate
// the thunk for the target function, flip the page writable via
// VirtualProtect, and overwrite the function pointer.

#pragma once

#include <windows.h>

// ─────────────────────────────────────────────────────────────────────
// IAT hook — single function-pointer slot in one module's import table.
// ─────────────────────────────────────────────────────────────────────

class IATHook {
public:
    IATHook() = default;
    ~IATHook();

    // Configure the hook. Doesn't install yet.
    //   target_module : the module whose IAT we patch (e.g. a DLL
    //                   loaded in explorer.exe)
    //   import_dll    : the DLL that exports the function (e.g.
    //                   L"user32.dll")
    //   import_func   : the function name (e.g. "TrackPopupMenu")
    //   detour        : pointer to our replacement function
    IATHook& init(HMODULE target_module,
                  const wchar_t* import_dll,
                  const char*    import_func,
                  void*          detour);

    bool install();
    bool uninstall();
    bool installed() const { return _installed; }
    void* slot_address() const { return _slot; }

private:
    HMODULE        _target  = nullptr;
    const wchar_t* _dll     = nullptr;
    const char*    _func    = nullptr;
    void*          _detour  = nullptr;
    void**         _slot    = nullptr;   // the IAT entry's address
    void*          _orig    = nullptr;   // saved original pointer
    bool           _installed = false;
};


// ─────────────────────────────────────────────────────────────────────
// Bulk install / uninstall entry points called from DllMain.
//
// install_iat_hooks(): walk every loaded module in this process and
// install IAT hooks on:
//   - user32!TrackPopupMenu
//   - user32!TrackPopupMenuEx
//   - win32u!NtUserTrackPopupMenuEx
//
// install_loadlibrary_hook(): hook kernel32!LoadLibraryExW (and the
// LdrLoadDll equivalent) so newly-loaded modules get their IATs
// patched as soon as Windows finishes loading them.
// ─────────────────────────────────────────────────────────────────────

void install_iat_hooks();
void uninstall_iat_hooks();

void install_loadlibrary_hook();
void uninstall_loadlibrary_hook();
