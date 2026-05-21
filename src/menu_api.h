// menu_api.h
// Consumer-facing API surface for the Right-Click Menu API.
// This is the *only* header a consumer's menu.cpp needs to include.
//
// All consumer-visible names live here: the two macros (Selection,
// Submenu), the context globals (target, paths, extension, ...),
// the predicate helpers (extension_exists, ...), and the small
// types/enums.
//
// Everything under the "internal" comment section is not for consumer
// use — it's exposed only because the macros expand into it.

#pragma once

#include <string>
#include <vector>
#include <functional>
#include <initializer_list>


// ─────────────────────────────────────────────────────────────────────
// Export / import attribute. Three modes:
//
//   MENU_API_STATIC          — statically linked (test_harness.exe).
//                              No dllexport, no dllimport.
//   MENU_API_BUILDING_DLL    — we are compiling shell_extension.dll
//                              itself. Symbols get dllexport.
//   (neither defined)        — consumer (menu.dll). Symbols get
//                              dllimport.
// ─────────────────────────────────────────────────────────────────────

#if   defined(MENU_API_STATIC)
    #define MENU_API_DECL
#elif defined(MENU_API_BUILDING_DLL)
    #define MENU_API_DECL __declspec(dllexport)
#else
    #define MENU_API_DECL __declspec(dllimport)
#endif


// ─────────────────────────────────────────────────────────────────────
// Types
// ─────────────────────────────────────────────────────────────────────

enum class ClickTarget {
    File,
    Folder,
    DirectoryBackground,
    Desktop,
    Drive,
    MultiSelection,
    VirtualItem,
    Toolbar,
};

struct Modifiers {
    bool shift;
    bool ctrl;
    bool alt;
    bool win;
};

struct Point {
    int x;
    int y;
};


// ─────────────────────────────────────────────────────────────────────
// Context globals
//
// Populated by the host DLL before RegisterMenu() is called on each
// right-click. The consumer reads these; they should never write.
// ─────────────────────────────────────────────────────────────────────

extern MENU_API_DECL ClickTarget               target;
extern MENU_API_DECL std::vector<std::string>  paths;
extern MENU_API_DECL std::string               path;
extern MENU_API_DECL std::string               extension;
extern MENU_API_DECL std::vector<std::string>  extensions;
extern MENU_API_DECL std::string               parentFolder;
extern MENU_API_DECL int                       selectionCount;
extern MENU_API_DECL Modifiers                 modifiers;
extern MENU_API_DECL Point                     clickLocation;


// ─────────────────────────────────────────────────────────────────────
// Predicate helpers
// ─────────────────────────────────────────────────────────────────────

MENU_API_DECL bool extension_exists(std::initializer_list<const char*> exts);

// Glob match against `path`. Reserved for later.
MENU_API_DECL bool path_matches(const char* glob);


// ─────────────────────────────────────────────────────────────────────
// Folder-view actions
//
// Drive the active File Explorer window's IFolderView2: change view
// mode, set sort column, set group column. Each returns true on
// success; failures are logged but never throw.
// ─────────────────────────────────────────────────────────────────────

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

MENU_API_DECL bool set_view_mode(ViewMode);
MENU_API_DECL bool set_sort_by  (SortKey);
MENU_API_DECL bool set_group_by (GroupKey);


// ─────────────────────────────────────────────────────────────────────
// Internal — used by the macros below, not for direct consumer use.
// ─────────────────────────────────────────────────────────────────────

namespace _menu_internal {

    // Tag types. Optional shortcut / icon fields default to nullptr.
    struct SelectionTag {
        const char* text;
        const char* shortcut;   // nullable, e.g. "Ctrl+C"
        const char* icon;       // nullable, e.g. "copy.png"
    };
    struct SubmenuTag {
        const char* text;
        const char* icon;       // nullable
    };

    // Overloaded so the variadic macro accepts 1, 2, or 3 args.
    MENU_API_DECL SelectionTag selection_helper(const char* text);
    MENU_API_DECL SelectionTag selection_helper(const char* text,
                                                const char* shortcut);
    MENU_API_DECL SelectionTag selection_helper(const char* text,
                                                const char* shortcut,
                                                const char* icon);
    MENU_API_DECL SubmenuTag   submenu_helper  (const char* text);
    MENU_API_DECL SubmenuTag   submenu_helper  (const char* text,
                                                const char* icon);

    MENU_API_DECL void push_separator();

    MENU_API_DECL void operator+(SelectionTag tag, std::function<void()> body);
    MENU_API_DECL void operator+(SubmenuTag   tag, std::function<void()> body);

}  // namespace _menu_internal


// ─────────────────────────────────────────────────────────────────────
// Consumer macros
//
// Usage:
//   Selection("Run as Python") {
//       LaunchProcess("python.exe", path);
//   };
//
//   Submenu("Git") {
//       Selection("Status") { /* ... */ };
//       Selection("Pull")   { /* ... */ };
//   };
//
// Each macro expands to:  helper(text) + [&]() { user_body }
//
// That's a normal C++ expression-statement, so it needs a trailing
// semicolon — one ';' after every closing '}'. The temporary returned
// by helper(text) has an overloaded operator+ that grabs the lambda
// and registers it. The user never types lambda brackets, std::function,
// or template angle brackets — just the macro and a body and a ';'.
// ─────────────────────────────────────────────────────────────────────

//   Selection("Copy")
//   Selection("Copy", "Ctrl+C")
//   Selection("Copy", "Ctrl+C", "copy.png")
//   Submenu("View")
//   Submenu("View", "view.png")
#define Selection(...)  ::_menu_internal::selection_helper(__VA_ARGS__) + [&]()
#define Submenu(...)    ::_menu_internal::submenu_helper  (__VA_ARGS__) + [&]()
#define Separator()     ::_menu_internal::push_separator()


// ─────────────────────────────────────────────────────────────────────
// Entry point — the consumer MUST define this.
//
// The host DLL calls RegisterMenu() on every right-click, after
// populating the context globals above. The consumer fills the body
// with normal C++ control flow (if / switch / for) that conditionally
// declares Selection / Submenu entries.
// ─────────────────────────────────────────────────────────────────────

extern "C" __declspec(dllexport) void RegisterMenu();
