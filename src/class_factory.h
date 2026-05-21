// class_factory.h
// Minimal IClassFactory + IContextMenu + IShellExtInit implementations,
// plus the CLSID identifying this shell extension.
//
// Why this exists: explorer.exe only loads a shell extension DLL when
// something asks for its registered CLSID. Our hooks live in DllMain,
// so as long as the DLL gets loaded our menu replacement works. The
// IContextMenu implementation is intentionally empty — it adds zero
// items to explorer's legacy menu. The hooks do the actual work.

#pragma once

#include <windows.h>
#include <unknwn.h>

// {7C3F4C5E-9D2E-4F7A-B5C8-1A3D4E5F6A7B}
// Fixed CLSID for the Right-Click Menu API shell extension. Mirrored
// in register.bat / unregister.bat.
extern "C" const GUID CLSID_RightClickMenuAPI;

// Entry point used by DllGetClassObject.
HRESULT CreateRightClickMenuClassFactory(REFIID riid, void** ppv);

// Outstanding-object counter — DllCanUnloadNow consults this.
LONG rcm_global_object_count();
