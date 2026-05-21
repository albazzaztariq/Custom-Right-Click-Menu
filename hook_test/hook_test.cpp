// hook_test.cpp
// Loads shell_extension.dll, queries which IAT slot it patched,
// reads that slot at multiple points, and calls TrackPopupMenuEx.

#include <windows.h>
#include <iostream>

int main() {
    HMODULE dll = ::LoadLibraryW(L"shell_extension.dll");
    if (!dll) {
        std::cout << "Failed to load shell_extension.dll. GLE="
                  << ::GetLastError() << "\n";
        std::cin.get();
        return 1;
    }

    using get_slot_t = void* (*)();
    auto get_slot = reinterpret_cast<get_slot_t>(
        ::GetProcAddress(dll, "get_patched_slot_address"));
    if (!get_slot) {
        std::cout << "get_patched_slot_address not exported.\n";
        std::cin.get();
        return 1;
    }

    void** slot = reinterpret_cast<void**>(get_slot());
    if (!slot) {
        std::cout << "Hook did not record a patched slot.\n";
        std::cin.get();
        return 1;
    }

    auto* vslot = reinterpret_cast<volatile void* const*>(slot);

    std::cout << "Patched slot address: " << slot << "\n";
    std::cout << "Slot value right after DllMain: " << *vslot << "\n";

    HMENU menu = ::CreatePopupMenu();
    ::AppendMenuW(menu, MF_STRING, 1, L"(placeholder)");

    std::cout << "Slot value right before TPME call: " << *vslot << "\n";

    int sw = ::GetSystemMetrics(SM_CXSCREEN);
    int sh = ::GetSystemMetrics(SM_CYSCREEN);
    int x = sw / 2;
    int y = sh / 2;
    std::cout << "Calling TrackPopupMenuEx at screen-center (" << x
              << ", " << y << ")\n";

    BOOL result = ::TrackPopupMenuEx(menu,
                                     TPM_LEFTALIGN | TPM_TOPALIGN,
                                     x, y,
                                     ::GetDesktopWindow(),
                                     nullptr);

    std::cout << "TrackPopupMenuEx returned " << result << "\n";
    std::cout << "Slot value after call:          " << *vslot << "\n";
    std::cout << "Press Enter to exit.\n";
    std::cin.get();

    ::DestroyMenu(menu);
    ::FreeLibrary(dll);
    return 0;
}
