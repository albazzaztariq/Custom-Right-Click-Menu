// main.cpp
// DllMain + COM/CLSID registration boilerplate for the shell extension
// DLL host. This is the file Windows loads into explorer.exe when it
// resolves the InProcServer32 for our registered CLSID.
//
// Scope of this file:
//   - DllMain: process-attach / detach lifecycle
//   - DllGetClassObject / DllCanUnloadNow: the required COM exports
//   - On first attach, load the consumer's menu.dll and grab a pointer
//     to its exported RegisterMenu() function
//
// Hooks (IAT / Detours) are installed from this file too, but the
// hook implementations live in hook.cpp and detours_hook.cpp.

#include <windows.h>
#include <olectl.h>   // SELFREG_E_CLASS
#include <string>

#include "menu_node.h"   // for reset_menu_state, handlerRegistry, rootMenu
#include "context.h"     // for reset_context, set_*
#include "hook.h"        // IAT hook installer (declared, stubbed)
#include "detours_hook.h"// CoCreateInstance hook (declared, stubbed)
#include "class_factory.h" // CLSID + IClassFactory for the shell extension
#include "diag.h"

// Function pointer type matching the consumer's RegisterMenu export.
using RegisterMenuFn = void (*)();

namespace {
    HMODULE        g_hInstance       = nullptr;
    HMODULE        g_consumerModule  = nullptr;
    RegisterMenuFn g_registerMenu    = nullptr;

    // Resolve menu.dll relative to our own DLL's location, so it works
    // regardless of which process loaded us (test harness, hook_test,
    // or explorer.exe).
    std::wstring find_consumer_dll_path() {
        wchar_t buf[MAX_PATH] = {};
        ::GetModuleFileNameW(g_hInstance, buf, MAX_PATH);
        std::wstring p(buf);
        auto slash = p.find_last_of(L"\\/");
        if (slash != std::wstring::npos) p.resize(slash + 1);
        p += L"menu.dll";
        return p;
    }

    bool load_consumer_dll() {
        if (g_consumerModule) return true;

        std::wstring p = find_consumer_dll_path();
        diag::log("load_consumer_dll: looking for %ls", p.c_str());

        g_consumerModule = ::LoadLibraryW(p.c_str());
        if (!g_consumerModule) {
            diag::log("  LoadLibraryW failed, GetLastError=%lu",
                      ::GetLastError());
            return false;
        }

        g_registerMenu = reinterpret_cast<RegisterMenuFn>(
            ::GetProcAddress(g_consumerModule, "RegisterMenu"));

        if (!g_registerMenu) {
            diag::log("  GetProcAddress(RegisterMenu) failed, "
                      "GetLastError=%lu", ::GetLastError());
        } else {
            diag::log("  RegisterMenu resolved at %p", g_registerMenu);
        }

        return g_registerMenu != nullptr;
    }

    void unload_consumer_dll() {
        if (g_consumerModule) {
            ::FreeLibrary(g_consumerModule);
            g_consumerModule = nullptr;
            g_registerMenu   = nullptr;
        }
    }
}


// ─────────────────────────────────────────────────────────────────────
// Entry point we call from the hook layer once per right-click.
//
// Sequence:
//   1) Reset per-click state.
//   2) Populate context globals from the shell's data. (TODO: real
//      population happens in the hook handler, this function assumes
//      the caller has done it.)
//   3) Call the consumer's RegisterMenu() — builds the tree.
//   4) Hand the resulting rootMenu back to the menu window for
//      rendering. (Done by the caller.)
// ─────────────────────────────────────────────────────────────────────

extern "C" void build_menu_for_current_click() {
    __try {
        _menu_internal::reset_menu_state();
        if (g_registerMenu) {
            diag::log("build_menu_for_current_click: calling RegisterMenu");
            g_registerMenu();
            diag::log("  RegisterMenu returned, top-level items=%zu",
                      _menu_internal::rootMenu.children.size());
        } else {
            diag::log("build_menu_for_current_click: no g_registerMenu!");
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        // Consumer code faulted. Leave rootMenu in whatever partial
        // state it was in; the menu window will show whatever it has,
        // or fall back to the original menu if it has nothing. Never
        // crash explorer.
    }
}


// ─────────────────────────────────────────────────────────────────────
// COM exports — minimum required for an in-process shell extension.
// ─────────────────────────────────────────────────────────────────────

extern "C" STDAPI DllGetClassObject(REFCLSID rclsid, REFIID riid, LPVOID* ppv) {
    if (ppv) *ppv = nullptr;
    if (rclsid != CLSID_RightClickMenuAPI) {
        return CLASS_E_CLASSNOTAVAILABLE;
    }
    return CreateRightClickMenuClassFactory(riid, ppv);
}

extern "C" STDAPI DllCanUnloadNow() {
    // We have hooks installed plus possibly-live COM objects. Refuse
    // unload until both refcount AND hook state are clean. For the MVP
    // we never unload — the cost of leaving the DLL resident is small,
    // and unloading mid-process while hooks are live would crash
    // explorer.
    if (rcm_global_object_count() != 0) return S_FALSE;
    return S_FALSE;
}


// ─────────────────────────────────────────────────────────────────────
// Self-registration helpers — written here so regsvr32.exe sees the
// exports. Schema below is the legacy "Context Menu Handler" pattern:
//
//   HKCR\CLSID\{guid}\(Default)              = "Right-Click Menu API"
//   HKCR\CLSID\{guid}\InProcServer32\(Default) = <path to this DLL>
//   HKCR\CLSID\{guid}\InProcServer32\ThreadingModel = "Apartment"
//   HKCR\*\shellex\ContextMenuHandlers\RightClickMenuAPI\(Default) = "{guid}"
//   HKCR\Directory\Background\shellex\ContextMenuHandlers\... etc.
//
// On Win10/11 the registry write under HKCR requires admin elevation
// (regsvr32 will UAC-prompt). DllUnregisterServer reverses everything.
// ─────────────────────────────────────────────────────────────────────

namespace {

constexpr wchar_t kHandlerName[] = L"RightClickMenuAPI";
constexpr wchar_t kFriendlyName[] = L"Right-Click Menu API";

std::wstring guid_to_string(REFGUID g) {
    wchar_t buf[64] = {};
    ::StringFromGUID2(g, buf, 64);
    return buf;  // includes braces
}

LONG write_string_value(HKEY root, const wchar_t* subkey,
                        const wchar_t* value_name, const wchar_t* data) {
    HKEY h = nullptr;
    LONG rc = ::RegCreateKeyExW(root, subkey, 0, nullptr,
                                REG_OPTION_NON_VOLATILE, KEY_WRITE,
                                nullptr, &h, nullptr);
    if (rc != ERROR_SUCCESS) return rc;
    rc = ::RegSetValueExW(h, value_name, 0, REG_SZ,
                          reinterpret_cast<const BYTE*>(data),
                          static_cast<DWORD>((wcslen(data) + 1)
                                             * sizeof(wchar_t)));
    ::RegCloseKey(h);
    return rc;
}

void register_handler_for(const wchar_t* asset_root,
                          const std::wstring& guid_str) {
    // asset_root is e.g. L"*" or L"Directory\\Background"
    std::wstring sub = asset_root;
    sub += L"\\shellex\\ContextMenuHandlers\\";
    sub += kHandlerName;
    write_string_value(HKEY_CLASSES_ROOT, sub.c_str(), nullptr,
                       guid_str.c_str());
}

void unregister_handler_for(const wchar_t* asset_root) {
    std::wstring sub = asset_root;
    sub += L"\\shellex\\ContextMenuHandlers\\";
    sub += kHandlerName;
    ::RegDeleteKeyW(HKEY_CLASSES_ROOT, sub.c_str());
}

}  // namespace

extern "C" STDAPI DllRegisterServer() {
    wchar_t dll_path[MAX_PATH] = {};
    if (!::GetModuleFileNameW(g_hInstance, dll_path, MAX_PATH)) {
        return HRESULT_FROM_WIN32(::GetLastError());
    }

    std::wstring guid_str = guid_to_string(CLSID_RightClickMenuAPI);

    // 1. HKCR\CLSID\{guid}
    std::wstring clsid_key = L"CLSID\\" + guid_str;
    if (write_string_value(HKEY_CLASSES_ROOT, clsid_key.c_str(),
                           nullptr, kFriendlyName) != ERROR_SUCCESS) {
        return SELFREG_E_CLASS;
    }

    // 2. HKCR\CLSID\{guid}\InProcServer32
    std::wstring inproc_key = clsid_key + L"\\InProcServer32";
    if (write_string_value(HKEY_CLASSES_ROOT, inproc_key.c_str(),
                           nullptr, dll_path) != ERROR_SUCCESS) {
        return SELFREG_E_CLASS;
    }
    write_string_value(HKEY_CLASSES_ROOT, inproc_key.c_str(),
                       L"ThreadingModel", L"Apartment");

    // 3. Asset-type associations. Register on every shell scope so our
    // DLL gets loaded regardless of where the right-click happens.
    register_handler_for(L"*",                       guid_str);
    register_handler_for(L"AllFilesystemObjects",    guid_str);
    register_handler_for(L"Directory",               guid_str);
    register_handler_for(L"Directory\\Background",   guid_str);
    register_handler_for(L"Drive",                   guid_str);
    register_handler_for(L"Folder",                  guid_str);

    // 4. Approved-handlers list (some Group Policy configurations
    // require this; harmless to skip when policy isn't set).
    write_string_value(
        HKEY_LOCAL_MACHINE,
        L"Software\\Microsoft\\Windows\\CurrentVersion\\"
        L"Shell Extensions\\Approved",
        guid_str.c_str(), kFriendlyName);

    return S_OK;
}

extern "C" STDAPI DllUnregisterServer() {
    std::wstring guid_str = guid_to_string(CLSID_RightClickMenuAPI);

    unregister_handler_for(L"*");
    unregister_handler_for(L"AllFilesystemObjects");
    unregister_handler_for(L"Directory");
    unregister_handler_for(L"Directory\\Background");
    unregister_handler_for(L"Drive");
    unregister_handler_for(L"Folder");

    std::wstring inproc_key = L"CLSID\\" + guid_str + L"\\InProcServer32";
    ::RegDeleteKeyW(HKEY_CLASSES_ROOT, inproc_key.c_str());

    std::wstring clsid_key = L"CLSID\\" + guid_str;
    ::RegDeleteKeyW(HKEY_CLASSES_ROOT, clsid_key.c_str());

    // Approved-handlers entry: delete the named value, leave the key.
    HKEY h = nullptr;
    if (::RegOpenKeyExW(HKEY_LOCAL_MACHINE,
            L"Software\\Microsoft\\Windows\\CurrentVersion\\"
            L"Shell Extensions\\Approved",
            0, KEY_WRITE, &h) == ERROR_SUCCESS) {
        ::RegDeleteValueW(h, guid_str.c_str());
        ::RegCloseKey(h);
    }

    return S_OK;
}


// ─────────────────────────────────────────────────────────────────────
// DllMain — process attach/detach lifecycle.
// ─────────────────────────────────────────────────────────────────────

BOOL APIENTRY DllMain(HMODULE hModule, DWORD reason, LPVOID /*reserved*/) {
    switch (reason) {

    case DLL_PROCESS_ATTACH:
        g_hInstance = hModule;
        ::DisableThreadLibraryCalls(hModule);

        __try {
            diag::log("=== DLL_PROCESS_ATTACH in PID %lu ===",
                      ::GetCurrentProcessId());

            // 1) Load the consumer's menu.dll and resolve RegisterMenu.
            //    Tolerate failure — without it we just don't show our
            //    menu, but explorer stays alive.
            load_consumer_dll();

            // 2) Install IAT hooks on TrackPopupMenu* across every
            //    currently-loaded module in this process.
            install_iat_hooks();

            // 3) Install the LoadLibrary hook so newly-loaded modules
            //    get their IATs patched on the fly.
            install_loadlibrary_hook();

            // 4) Install Detours hook on CoCreateInstance to refuse
            //    Win11 modern-menu COM activation.
            install_cocreateinstance_hook();
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            // Anything blowing up during init must not take explorer
            // down with it. Best-effort: keep the DLL loaded but
            // inert.
        }
        break;

    case DLL_PROCESS_DETACH:
        __try {
            uninstall_cocreateinstance_hook();
            uninstall_loadlibrary_hook();
            uninstall_iat_hooks();
            unload_consumer_dll();
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            // Same principle on shutdown.
        }
        break;

    }
    return TRUE;
}
