// test_harness.cpp
// Standalone exe that exercises the menu API directly — no shell
// extension, no explorer.exe, no CLSID registration. Builds, runs,
// shows a menu at the cursor. Click an item, its handler runs.
//
// This is how you verify the consumer-facing API works before the
// shell-extension plumbing is wired up.

#include "../src/menu_api.h"
#include "../src/menu_node.h"
#include "../src/context.h"
#include "../src/menu_window.h"

#include <windows.h>
#include <iostream>
#include <string>

// ─────────────────────────────────────────────────────────────────────
// Consumer-equivalent code. Same shape an end-user would write.
// Note the trailing semicolon after each Selection/Submenu block —
// each one is an expression-statement, so it needs ';' to terminate.
// ─────────────────────────────────────────────────────────────────────

void RegisterMenu() {

    Selection("Say hello") {
        ::MessageBoxA(nullptr,
                      ("Hello!\n\npath = " + path).c_str(),
                      "Menu API test", MB_OK);
    };

    Selection("Copy path to clipboard") {
        if (::OpenClipboard(nullptr)) {
            ::EmptyClipboard();
            HGLOBAL h = ::GlobalAlloc(GMEM_MOVEABLE, path.size() + 1);
            if (h) {
                if (auto* p = static_cast<char*>(::GlobalLock(h))) {
                    memcpy(p, path.c_str(), path.size() + 1);
                    ::GlobalUnlock(h);
                    ::SetClipboardData(CF_TEXT, h);
                }
            }
            ::CloseClipboard();
        }
    };

    if (target == ClickTarget::File && extension_exists({".py", ".pyx"})) {
        Selection("Run as Python") {
            ::MessageBoxA(nullptr, "Would run Python here.",
                          "Menu API test", MB_OK);
        };
    }

    Submenu("Nested submenu") {
        Selection("Item A") {
            ::MessageBoxA(nullptr, "You picked A.",
                          "Menu API test", MB_OK);
        };
        Selection("Item B") {
            ::MessageBoxA(nullptr, "You picked B.",
                          "Menu API test", MB_OK);
        };
        Submenu("Even deeper") {
            Selection("Deep item 1") {
                ::MessageBoxA(nullptr, "Deep 1.",
                              "Menu API test", MB_OK);
            };
            Selection("Deep item 2") {
                ::MessageBoxA(nullptr, "Deep 2.",
                              "Menu API test", MB_OK);
            };
        };
    };
}


// ─────────────────────────────────────────────────────────────────────
// Harness main.
// ─────────────────────────────────────────────────────────────────────

int main() {
    _menu_internal::reset_context();
    _menu_internal::set_paths({"C:\\test\\example.py"});
    _menu_internal::set_target(ClickTarget::File);
    _menu_internal::set_modifiers(false, false, false, false);

    _menu_internal::reset_menu_state();
    RegisterMenu();

    std::cout
        << "Built menu with "
        << _menu_internal::rootMenu.children.size()
        << " top-level items.\n";

    POINT pt{};
    ::GetCursorPos(&pt);
    std::cout << "Showing menu at (" << pt.x << ", " << pt.y << ").\n"
              << "Left-click an item, or press ESC / right-click to dismiss.\n";

    int clicked = menu_window::show(_menu_internal::rootMenu,
                                    pt.x, pt.y, nullptr);

    if (clicked >= 0 &&
        clicked < (int)_menu_internal::handlerRegistry.size()) {
        std::cout << "Dispatching handler " << clicked << "...\n";
        try {
            _menu_internal::handlerRegistry[clicked]();
        } catch (...) {
            std::cout << "Handler threw an exception.\n";
        }
    } else {
        std::cout << "Menu dismissed without selection.\n";
    }

    std::cout << "Press Enter to exit.\n";
    std::cin.get();
    return 0;
}
