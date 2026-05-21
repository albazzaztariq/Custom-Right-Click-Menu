// context.h
// Declares the context globals the consumer reads at right-click time.
// These are defined in context.cpp and populated by the host DLL
// before RegisterMenu() is called.
//
// Note: the same names are declared `extern` in menu_api.h for
// consumer consumption. This header is for the host side only.

#pragma once

#include "menu_api.h"  // for ClickTarget, Modifiers, Point

// All globals are defined in context.cpp.

namespace _menu_internal {

    // Reset every context global to a sensible empty/default state.
    // Called by the host before populating fresh values per click.
    void reset_context();

    // Convenience helpers the host uses to populate context from the
    // shell's IShellView / IContextMenu data. Definitions live in
    // context.cpp; the host calls these once per right-click.
    void set_paths(std::vector<std::string> p);
    void set_click_location(int x, int y);
    void set_modifiers(bool shift, bool ctrl, bool alt, bool win);
    void set_target(ClickTarget t);

}  // namespace _menu_internal
