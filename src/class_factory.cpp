// class_factory.cpp
// Stage-3 plumbing. Implements just enough COM machinery for explorer
// to instantiate our "context menu handler" and consequently load this
// DLL into its process. The handler itself adds zero menu items — the
// actual menu replacement is done by the IAT/inline hooks installed in
// DllMain.

#include "class_factory.h"
#include "diag.h"

#include <shlobj.h>
#include <new>

// CLSID definition. Keep the bytes in sync with class_factory.h.
extern "C" const GUID CLSID_RightClickMenuAPI =
    { 0x7C3F4C5E, 0x9D2E, 0x4F7A,
      { 0xB5, 0xC8, 0x1A, 0x3D, 0x4E, 0x5F, 0x6A, 0x7B } };

static LONG g_object_count = 0;
LONG rcm_global_object_count() { return g_object_count; }


// ─────────────────────────────────────────────────────────────────────
// IShellExtInit + IContextMenu — empty implementation.
//
// QueryContextMenu returns "added 0 items" so explorer keeps building
// its own menu. We're here for the side-effect of having been loaded.
// ─────────────────────────────────────────────────────────────────────

namespace {

class RcmContextMenu : public IShellExtInit, public IContextMenu {
public:
    RcmContextMenu() : _ref(1) {
        ::InterlockedIncrement(&g_object_count);
    }
    virtual ~RcmContextMenu() {
        ::InterlockedDecrement(&g_object_count);
    }

    // IUnknown
    IFACEMETHODIMP QueryInterface(REFIID riid, void** ppv) override {
        if (!ppv) return E_POINTER;
        if (riid == IID_IUnknown || riid == IID_IShellExtInit) {
            *ppv = static_cast<IShellExtInit*>(this);
        } else if (riid == IID_IContextMenu) {
            *ppv = static_cast<IContextMenu*>(this);
        } else {
            *ppv = nullptr;
            return E_NOINTERFACE;
        }
        AddRef();
        return S_OK;
    }
    IFACEMETHODIMP_(ULONG) AddRef()  override {
        return ::InterlockedIncrement(&_ref);
    }
    IFACEMETHODIMP_(ULONG) Release() override {
        LONG r = ::InterlockedDecrement(&_ref);
        if (r == 0) delete this;
        return r;
    }

    // IShellExtInit — explorer passes the clicked-item data here; we
    // don't need it (the hook layer reads the selection independently
    // via IShellWindows). Accept and forget.
    IFACEMETHODIMP Initialize(LPCITEMIDLIST /*pidl*/,
                              IDataObject*  /*pdo*/,
                              HKEY          /*hk*/) override {
        return S_OK;
    }

    // IContextMenu — add zero items, return SEVERITY_SUCCESS with count 0.
    IFACEMETHODIMP QueryContextMenu(HMENU /*hmenu*/, UINT /*indexMenu*/,
                                    UINT /*idCmdFirst*/, UINT /*idCmdLast*/,
                                    UINT /*uFlags*/) override {
        return MAKE_HRESULT(SEVERITY_SUCCESS, 0, 0);
    }
    IFACEMETHODIMP InvokeCommand(CMINVOKECOMMANDINFO* /*pi*/) override {
        return E_FAIL;
    }
    IFACEMETHODIMP GetCommandString(UINT_PTR /*idCmd*/, UINT /*uFlags*/,
                                    UINT* /*pwReserved*/, LPSTR /*pszName*/,
                                    UINT /*cchMax*/) override {
        return E_NOTIMPL;
    }

private:
    LONG _ref;
};


// ─────────────────────────────────────────────────────────────────────
// IClassFactory
// ─────────────────────────────────────────────────────────────────────

class RcmClassFactory : public IClassFactory {
public:
    RcmClassFactory() : _ref(1) {
        ::InterlockedIncrement(&g_object_count);
    }
    virtual ~RcmClassFactory() {
        ::InterlockedDecrement(&g_object_count);
    }

    IFACEMETHODIMP QueryInterface(REFIID riid, void** ppv) override {
        if (!ppv) return E_POINTER;
        if (riid == IID_IUnknown || riid == IID_IClassFactory) {
            *ppv = static_cast<IClassFactory*>(this);
            AddRef();
            return S_OK;
        }
        *ppv = nullptr;
        return E_NOINTERFACE;
    }
    IFACEMETHODIMP_(ULONG) AddRef()  override {
        return ::InterlockedIncrement(&_ref);
    }
    IFACEMETHODIMP_(ULONG) Release() override {
        LONG r = ::InterlockedDecrement(&_ref);
        if (r == 0) delete this;
        return r;
    }

    IFACEMETHODIMP CreateInstance(IUnknown* outer, REFIID riid,
                                  void** ppv) override {
        if (outer) return CLASS_E_NOAGGREGATION;
        if (!ppv)  return E_POINTER;
        *ppv = nullptr;

        RcmContextMenu* obj = new (std::nothrow) RcmContextMenu();
        if (!obj) return E_OUTOFMEMORY;

        HRESULT hr = obj->QueryInterface(riid, ppv);
        obj->Release();
        return hr;
    }
    IFACEMETHODIMP LockServer(BOOL lock) override {
        if (lock) ::InterlockedIncrement(&g_object_count);
        else      ::InterlockedDecrement(&g_object_count);
        return S_OK;
    }

private:
    LONG _ref;
};

}  // namespace


HRESULT CreateRightClickMenuClassFactory(REFIID riid, void** ppv) {
    if (!ppv) return E_POINTER;
    *ppv = nullptr;

    RcmClassFactory* cf = new (std::nothrow) RcmClassFactory();
    if (!cf) return E_OUTOFMEMORY;

    HRESULT hr = cf->QueryInterface(riid, ppv);
    cf->Release();
    return hr;
}
