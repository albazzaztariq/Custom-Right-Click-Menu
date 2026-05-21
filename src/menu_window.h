// menu_window.h
// Owner-drawn window that displays our menu tree.
//
// Called by the TrackPopupMenu* detours in hook.cpp when explorer
// asks to show a menu. Replaces the system menu surface entirely.

#pragma once

#include <windows.h>
#include "menu_node.h"

namespace menu_window {

    // Show the menu rooted at `root` at screen coordinates (x, y).
    // Returns the handlerId of the clicked Selection, or -1 if the
    // user dismissed the menu without choosing anything.
    int show(const MenuNode& root, int x, int y, HWND owner);

}  // namespace menu_window
