// detours_hook.cpp
// CoCreateInstance hook — refuses to construct Win11 modern-menu COM
// objects so explorer falls back to the legacy TrackPopupMenu path
// (which our hook.cpp intercepts).
//
// Naming note: this file is named after Microsoft Detours from the
// original scaffolding, but it does not depend on the Detours library.
// The IAT-hooking primitive in hook.cpp is sufficient — we use it here
// to patch ole32!CoCreateInstance and combase!CoCreateInstance across
// every loaded module, with a saved-original pointer for call-through.
//
// Extending the refusal list: add a CLSID to kRefusedClsids below. To
// discover which CLSIDs explorer asks for, set kLogEveryCci = true and
// reproduce a right-click — every CoCreateInstance call gets logged
// with its CLSID, IID, and the resulting HRESULT.

#include "detours_hook.h"
#include "hook.h"   // IATHook
#include "diag.h"

#include <windows.h>
#include <objbase.h>
#include <psapi.h>
#include <vector>

#pragma comment(lib, "ole32.lib")

// Set to true to log every CoCreateInstance call. Useful for
// discovering which CLSIDs to add to kRefusedClsids. Costs ~one log
// line per COM activation, so leave off in steady state.
static constexpr bool kLogEveryCci = false;

// CLSIDs to refuse. When explorer asks for one of these, the detour
// returns REGDB_E_CLASSNOTREG, which causes explorer to skip the
// modern-menu code path and fall through to the legacy menu builder.
//
// Seed with placeholder examples — actual Win11-modern-menu CLSIDs
// vary by Windows build. Use kLogEveryCci = true to discover them in
// situ.
static const GUID kRefusedClsids[] = {
    // {e2bf9676-5f8f-435c-97eb-11607a5bedf7} — example placeholder.
    { 0xe2bf9676, 0x5f8f, 0x435c,
      { 0x97, 0xeb, 0x11, 0x60, 0x7a, 0x5b, 0xed, 0xf7 } },
};

using CoCreateInstance_t = HRESULT (WINAPI*)(REFCLSID, LPUNKNOWN, DWORD,
                                             REFIID, LPVOID*);

static CoCreateInstance_t g_orig_CoCreateInstance = nullptr;
static std::vector<IATHook> g_cci_hooks;

static bool guid_in(const GUID& g, const GUID* arr, size_t n) {
    for (size_t i = 0; i < n; ++i) {
        if (::IsEqualGUID(g, arr[i])) return true;
    }
    return false;
}

static HRESULT WINAPI CoCreateInstance_detour(REFCLSID rclsid,
                                              LPUNKNOWN punk,
                                              DWORD ctx,
                                              REFIID riid,
                                              LPVOID* ppv) {
    if (guid_in(rclsid, kRefusedClsids,
                sizeof(kRefusedClsids) / sizeof(kRefusedClsids[0]))) {
        if (ppv) *ppv = nullptr;
        wchar_t buf[64] = {};
        ::StringFromGUID2(rclsid, buf, 64);
        diag::log("CoCreateInstance_detour: refused CLSID %ls", buf);
        return REGDB_E_CLASSNOTREG;
    }

    if (!g_orig_CoCreateInstance) return E_FAIL;
    HRESULT hr = g_orig_CoCreateInstance(rclsid, punk, ctx, riid, ppv);

    if (kLogEveryCci) {
        wchar_t cbuf[64] = {}, ibuf[64] = {};
        ::StringFromGUID2(rclsid, cbuf, 64);
        ::StringFromGUID2(riid, ibuf, 64);
        diag::log("CoCreateInstance: clsid=%ls iid=%ls -> hr=0x%08x",
                  cbuf, ibuf, hr);
    }
    return hr;
}

namespace {

std::vector<HMODULE> enumerate_modules() {
    std::vector<HMODULE> result;
    HANDLE proc = ::GetCurrentProcess();
    DWORD needed = 0;
    if (!::EnumProcessModules(proc, nullptr, 0, &needed)) return result;
    result.resize(needed / sizeof(HMODULE));
    if (!::EnumProcessModules(proc, result.data(),
                              static_cast<DWORD>(result.size()
                                                 * sizeof(HMODULE)),
                              &needed)) {
        return {};
    }
    return result;
}

// Hook the CoCreateInstance import in every loaded module that imports
// it from `dll_name`.
void hook_cci_from(const wchar_t* dll_name,
                   const std::vector<HMODULE>& modules) {
    int n = 0;
    for (HMODULE m : modules) {
        IATHook h;
        h.init(m, dll_name, "CoCreateInstance",
               reinterpret_cast<void*>(&CoCreateInstance_detour));
        if (h.install()) {
            g_cci_hooks.push_back(std::move(h));
            ++n;
        }
    }
    diag::log("install_cocreateinstance_hook: %d IAT hooks via %ls",
              n, dll_name);
}

}  // namespace

void install_cocreateinstance_hook() {
    // Resolve the real CoCreateInstance via GetProcAddress. Modern
    // Windows forwards ole32!CoCreateInstance to combase, but
    // GetProcAddress chases the forwarder so we get the actual entry.
    HMODULE ole = ::GetModuleHandleW(L"ole32.dll");
    HMODULE comb = ::GetModuleHandleW(L"combase.dll");
    if (ole) {
        g_orig_CoCreateInstance = reinterpret_cast<CoCreateInstance_t>(
            ::GetProcAddress(ole, "CoCreateInstance"));
    }
    if (!g_orig_CoCreateInstance && comb) {
        g_orig_CoCreateInstance = reinterpret_cast<CoCreateInstance_t>(
            ::GetProcAddress(comb, "CoCreateInstance"));
    }
    if (!g_orig_CoCreateInstance) {
        diag::log("install_cocreateinstance_hook: no real "
                  "CoCreateInstance found; skipping");
        return;
    }

    auto modules = enumerate_modules();
    hook_cci_from(L"ole32.dll",    modules);
    hook_cci_from(L"combase.dll",  modules);
}

void uninstall_cocreateinstance_hook() {
    for (auto& h : g_cci_hooks) h.uninstall();
    g_cci_hooks.clear();
    g_orig_CoCreateInstance = nullptr;
}
