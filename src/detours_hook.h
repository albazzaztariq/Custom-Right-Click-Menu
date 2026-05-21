// detours_hook.h
// Microsoft Detours hook on CoCreateInstance.
//
// Purpose: refuse to construct the Win11 modern-menu COM objects, so
// explorer falls back to the legacy TrackPopupMenu path (which is
// already hooked in hook.cpp / hook.h).

#pragma once

void install_cocreateinstance_hook();
void uninstall_cocreateinstance_hook();
