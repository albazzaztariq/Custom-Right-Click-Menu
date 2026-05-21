// shell_context.h
// Stage-4 helper: query explorer for the actual right-click context
// (which window, which folder, which selection) and populate the
// _menu_internal::set_* helpers in context.cpp.
//
// Lives in its own translation unit because it pulls in the heavy
// shell COM headers (shlobj, exdisp, shobjidl) that we don't want
// leaking into hook.cpp.

#pragma once

#include <windows.h>

// Populate context globals from explorer's live state. `owner_hwnd` is
// the HWND passed to TrackPopupMenuEx. `screen_x` / `screen_y` are the
// click coordinates (also stored as clickLocation).
//
// Returns true if we successfully extracted real context; false if we
// couldn't find a matching shell window (caller should fall back to
// the legacy hardcoded behaviour). On failure, modifiers and click
// location are still populated.
bool populate_shell_context(HWND owner_hwnd, int screen_x, int screen_y);
