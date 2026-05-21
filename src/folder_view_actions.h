// folder_view_actions.h
// Internal helpers that drive the active File Explorer IFolderView2:
// change view mode (Large/Medium/Small icons, List, Details), set sort
// column, set group column.
//
// Implementation lives in folder_view_actions.cpp. The public entry
// points are declared in menu_api.h (set_view_mode / set_sort_by /
// set_group_by) and forward to these.

#pragma once

namespace _folder_view_actions {

enum class ViewMode {
    LargeIcons,
    MediumIcons,
    SmallIcons,
    List,
    Details,
};

enum class SortKey {
    Name,
    DateModified,
    Size,
    Type,
};

enum class GroupKey {
    Name,
    DateModified,
    Type,
    None,
};

// All three return true on success. SEH-wrapped internally — never
// propagate a fault. Resolve the active explorer window via the same
// IShellWindows walk shell_context.cpp uses, but match the foreground
// HWND instead of an explicit owner.
bool apply_view_mode(ViewMode);
bool apply_sort_by  (SortKey);
bool apply_group_by (GroupKey);

}  // namespace _folder_view_actions
