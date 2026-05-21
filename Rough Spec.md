# Right-Click Menu API — Rough Spec

This is an informal working document. It captures intent and the authoring
model. It does not yet specify implementation. The architecture writeup
(how Win11 interception actually works, verified against Nilesoft's source)
is a separate document that has not been written yet.


## Goal

Build a Windows 11 shell extension that **replaces** the right-click context
menu — not adds to it, not restores the legacy Win32 menu, not "Show more
options." When the user right-clicks anywhere shell-related (file, folder,
directory background, desktop), the system's menu is suppressed and a
custom menu rendered by this project appears instead.

The menu's contents are defined by the consumer of this project **in code**,
through a small API. No DSL, no .nss, no XML, no JSON required to declare
entries.


## The mental model — only two things exist

Every entry in the menu is exactly one of two types:

1. **Selection** — a leaf. When clicked, it runs the block of code attached
   to it. Nothing else happens; the menu closes.

2. **Submenu** — a container. When hovered, it opens a child menu. That
   child menu contains more entries, which are themselves either Selections
   or Submenus (recursively, no depth limit).

Nothing else. Separators and the like are presentational concerns that can
be added later; they do not change the mental model.


## Authoring syntax (the shape the user wants)

The end-state for a consumer file:

```
AddSelection("Open in VS Code") {
    // code that runs when this entry is clicked
}

AddSelection("Copy Path") {
    // ...
}

AddSubmenu("Git") {
    AddSelection("Status") {
        // ...
    }
    AddSelection("Pull") {
        // ...
    }

    AddSubmenu("Branch") {
        AddSelection("New branch") {
            // ...
        }
        AddSelection("Switch branch") {
            // ...
        }
    }
}
```

Reading rules for the above:

- `AddSelection(text) { body }` — registers a leaf entry with `text` as its
  label. The body runs when the entry is clicked.
- `AddSubmenu(text) { body }` — registers a submenu with `text` as its
  label. The body is a sequence of further `AddSelection` / `AddSubmenu`
  calls that populate this submenu.
- Order in the file = order in the menu.
- Nesting in the file = nesting in the menu.

The exact language and exact method names will be pinned in the tech-stack
decision (see Open Questions). The shape above is the target — whatever
language we pick has to be able to express it cleanly, with nested blocks
that look this much like a tree.


## What the click handler can see

When a Selection is clicked, the handler needs context. At a minimum:

- The list of selected paths at the moment of right-click (zero or more
  files / folders).
- Where the right-click happened: file, folder, directory background, or
  desktop.
- The screen-coordinate location of the click (for things that want to
  position their own UI nearby).
- Modifier keys held during the click (Shift, Ctrl, Alt).

How this is delivered (an explicit `ctx` argument vs. ambient/implicit
state inside the block) depends on the host language and is part of the
API design pass. For this spec it's enough to say: that information is
available to every handler.


## What gets suppressed

- The Windows 11 modern context menu (the new compact one).
- The "Show more options" legacy menu (we do not want to live inside it —
  our menu *is* the menu).
- Any other shell extension's entries are out of scope for now. Whether we
  can or want to also display them alongside our own is a later question.


## What this is NOT

- Not a configuration tool. There is no settings UI, no `.nss` file, no
  JSON, no XML for declaring menu entries. Those things can exist later
  as optional layers on top of the code API, but the core authoring model
  is code.
- Not an "add to the existing menu" extension. The existing menus do not
  appear.
- Not theming or visual customization right now. The menu has *some* look;
  what it looks like is a later concern.


## MVP — smallest thing that proves it works

Goal of the MVP: prove both that we can intercept the click AND that the
authoring API dispatches correctly.

Scope:

1. One consumer file containing exactly one line of menu definition:
   `AddSelection("Hello") { launch_notepad() }`
2. Right-click on the desktop → our custom menu appears with that one
   item, and **only** that one item.
3. The Win11 modern menu does not appear. The "Show more options" menu
   does not appear.
4. Clicking "Hello" launches notepad.exe.
5. Nothing else. No icons. No theming. No submenus in the MVP. No
   conditional visibility. No multiple right-click targets — desktop only.

If that works end-to-end, the architecture is real and the API shape is
real. Everything else is incremental.


## Architecture — how Nilesoft Shell does it (verified against source)

Read from github.com/moudey/Shell. File/line references are to that
repo's `main` branch at time of review.

**Shape.** Nilesoft is a single in-process DLL (`src/dll/`) registered
as a classic `IContextMenu` shell extension under
`HKEY_CLASSES_ROOT\CLSID\{...}\InProcServer32`. That registration is
what gets the DLL loaded into `explorer.exe` whenever a context menu is
about to be built. Standard shell extension plumbing. Nothing exotic
about how it gets loaded.

**What it does once inside.** `DllMain` (in `src/dll/src/Main.cpp`)
checks whether the host process is explorer.exe (using the constant
`def_EXPLORER`) or their own dev tool `shell.exe`. It then calls
`_loader.init()`, which installs hooks across every module already
loaded in the explorer process.

**The interception — three layers stacked.**

1. **IAT hooking** (custom, not MinHook/Detours). In
   `src/dll/src/Include/Hooker.h` and `Include/win32_hook.h` Nilesoft
   ships its own IAT (Import Address Table) hook implementation. The
   `IATHook` class walks PE headers (`IMAGE_DOS_HEADER`,
   `IMAGE_NT_HEADERS`, the import descriptor, the thunk table),
   locates the entry for the target function, flips the memory page
   writable via `VirtualProtect`, and overwrites the function pointer
   in place. They install IAT hooks on `user32!TrackPopupMenu` and
   `win32u!NtUserTrackPopupMenuEx` across every loaded module in
   explorer (loop over `Process::Modules(...)`). When explorer calls
   either of those functions to display its own context menu, Nilesoft's
   `TrackPopupMenuProc` / `TrackPopupMenuExProc` fires first, suppresses
   the original call, and shows its own owner-drawn window instead.
   The custom drawing happens in `src/dll/src/ContextMenu.cpp::OnDrawItem`.

2. **Microsoft Detours hook on `CoCreateInstance`** — the *only* third-
   party hooking library, linked as `detours-x64.lib`. This is the
   mechanism for killing the Win11 modern menu. The modern menu is
   instantiated through COM (Component Object Model). By intercepting
   `CoCreateInstance` they can refuse to construct the modern-menu
   objects, which forces explorer to fall back to the legacy
   `TrackPopupMenu` path — which is already hooked by layer 1, so their
   custom menu takes over.

3. **Window subclassing** for the taskbar — via the `WindowSubclass`
   class in `Hooker.h`. A `TaskbarSubclassProc` intercepts right-click
   messages on the taskbar window itself, which has its own menu
   pipeline distinct from explorer's file/folder context menus.

**Net effect.** Registration gets the DLL into explorer's address space.
IAT hooks replace the menu surface. The `CoCreateInstance` hook suppresses
the modern-menu COM activation so the legacy code path is the one that
actually runs. Three layers, each handling a different code path that
explorer can take to show a menu.

**What we can drop.** Nilesoft ships a lot we don't need for an MVP — a
custom DSL parser (`src/dll/src/Parser/`), an expression evaluator
(`src/dll/src/Expression/`), a full theme engine with acrylic/mica/blur,
an icon/image loader (`stb_image_write.h`), tooltip rendering, keyboard
shortcut handling. The pure interception-plus-rendering core is a
fraction of the codebase. Our project is the code-API version, which
also lets us drop the `.nss` parser entirely.

## API Surface (locked)

Single language: C++ throughout. Consumer writes a `menu.cpp` file that
includes one header (`menu_api.h`) and defines one entry-point function
(`RegisterMenu`). The macro layer hides the C++ syntax noise so the
consumer never types `[&]`, `std::function`, or template angle brackets.

### Consumer-facing macros

- `Selection(text) { body }` — leaf entry. `body` runs when clicked.
- `Submenu(text) { body }` — container. `body` contains nested
  `Selection` / `Submenu` declarations.

### Entry point (consumer must define)

- `void RegisterMenu()` — exported from `menu.dll`, called by our shell
  extension on every right-click. Runs top to bottom each click,
  building the full menu tree using normal `if`/`switch` control flow
  against the context globals below.

### Context globals (read-only, populated before `RegisterMenu()` runs)

- `ClickTarget target`
- `std::vector<std::string> paths`
- `std::string path` — single-selection convenience; empty if zero or
  many items selected
- `std::string extension` — clicked item's extension, lowercase, with
  leading dot (`".py"`). Empty if not a file.
- `std::vector<std::string> extensions` — every extension in a
  multi-selection
- `std::string parentFolder`
- `int selectionCount`
- `Modifiers modifiers`
- `Point clickLocation`

### Predicate helpers

- `bool extension_exists(std::initializer_list<const char*> exts)` —
  true if `extension` is one of the listed values
- `bool path_matches(const char* glob)` — glob match against `path`
  (later)

### Types / enums

```cpp
enum class ClickTarget {
    File, Folder, DirectoryBackground, Desktop, Drive,
    MultiSelection, VirtualItem, Toolbar
};
struct Modifiers { bool shift, ctrl, alt, win; };
struct Point     { int x, y; };
```

### Hidden infrastructure (behind the macros, lives in our DLL)

- `selection_helper(const char* text)` — what `Selection` expands to.
  Returns a small temporary whose `operator+` accepts the lambda the
  user wrote in `{ ... }` and registers it.
- `submenu_helper(const char* text)` — same idea for submenus. Pushes
  a new node onto the menu-build stack before running the lambda,
  pops afterwards.
- `thread_local MenuNode* currentMenu` — the "what menu am I adding
  into right now" pointer that makes nested `{ ... }` blocks build the
  correct tree.
- `std::vector<std::function<void()>> handlerRegistry` — stores every
  `Selection` body lambda, indexed by integer ID. Click dispatch looks
  up by ID.
- `struct MenuNode { std::string text; std::vector<MenuNode> children;
  int handlerId; };` — the tree node type. A node is either a Submenu
  (children, no handler) or a Selection (handler, no children).

### What the consumer file looks like

Note the trailing `;` after each `Selection` / `Submenu` closing brace.
The macro expands to an expression-statement; C++ requires `;` to
terminate it. The cost is one extra character per entry.

```cpp
#include "menu_api.h"

void RegisterMenu() {
    if (target == ClickTarget::File &&
        extension_exists({".py", ".pyx"})) {
        Selection("Run as Python") {
            LaunchProcess("python.exe", path);
        };
        Selection("Run tests") {
            LaunchProcess("pytest", parentFolder);
        };
    }

    if (target == ClickTarget::DirectoryBackground) {
        Submenu("New") {
            Selection("Folder") {
                CreateFolder(parentFolder + "\\New Folder");
            };
            Selection("Text file") { /* ... */ };
        };
    }

    Submenu("Git") {
        Selection("Status") { /* ... */ };
        Selection("Pull")   { /* ... */ };
    };
}
```


## Minimum Viable Build — the under-10% slice of Nilesoft

What we keep from Nilesoft's design, ported into our own minimal codebase:

**Source files (target, not actual):**

- `main.cpp` — `DllMain`, COM CLSID registration boilerplate, calls
  into the hook installer on `DLL_PROCESS_ATTACH`, calls cleanup on
  `DLL_PROCESS_DETACH`.
- `hook.h` / `hook.cpp` — IAT hook implementation (the `IATHook` class
  pattern from Nilesoft's `Hooker.h`: walk PE headers, find the thunk,
  `VirtualProtect` writable, swap the pointer).
- `detours_hook.cpp` — Microsoft Detours hook for `CoCreateInstance`,
  refusing to create the modern-menu COM objects so the legacy path
  takes over.
- `menu_window.cpp` / `.h` — minimal owner-drawn window class for our
  menu. No theming, no acrylic, no rounded corners, no icons. System
  defaults for font, background, text. Just functional.
- `entry.h` / `.cpp` — the `Selection` and `Submenu` data types, the
  tree builder, and dispatch into consumer handlers when an item is
  clicked.
- `api.h` — the consumer-facing surface: `AddSelection`, `AddSubmenu`,
  plus the click-time context globals (`target`, `extension`, etc.).
- `loader.cpp` — calls `consumer.dll`'s registration entry point so the
  consumer's menu file gets to run.

**Hooks we install:**

- `user32!TrackPopupMenu` — IAT patched.
- `win32u!NtUserTrackPopupMenuEx` — IAT patched.
- `ole32!CoCreateInstance` — via Microsoft Detours, suppresses the modern
  menu's COM activation.
- `kernel32!LoadLibraryExW` and `ntdll!LdrLoadDll` — IAT patched, so
  newly-loaded modules get their IAT patched before they execute (see
  the "late-loaded module" risk below).

**Explicitly dropped from Nilesoft:**

- `.nss` DSL parser (`src/dll/src/Parser/`)
- Expression evaluator (`src/dll/src/Expression/`)
- Theme engine: acrylic, mica, blur, gradient backgrounds, custom fonts
- Icon / image loader and `stb_image_write.h`
- Tooltip rendering (`Tip.h`)
- Keyboard shortcut handling (`Keyboard.h`)
- Taskbar context-menu subclassing (out of MVP scope — file/folder/
  desktop/background only)

The goal of the MVP is to prove the interception works and the API
dispatches. Visual polish, theming, taskbar, advanced predicates — all
later.


## Risks and failure modes

Where this approach can go wrong, in roughly decreasing order of
likelihood:

- **New modules loaded after hook install.** IAT (Import Address Table)
  hooks work by patching one specific module's import table. Every DLL
  loaded into explorer.exe has its own table listing the functions it
  imports from other DLLs. At startup we walk every currently-loaded
  module and patch each one. But explorer keeps loading DLLs throughout
  its lifetime — file format handlers, network plugins, UWP host
  components, third-party shell extensions, etc. Those new modules have
  unpatched IATs, so when one of *them* calls `TrackPopupMenu`, our hook
  doesn't see the call. **Mitigation:** also IAT-hook
  `kernel32!LoadLibraryExW` and `ntdll!LdrLoadDll`. When explorer loads
  a new DLL, our hook fires, we patch that DLL's IAT, then we let the
  original load complete. The newly-loaded module's first
  `TrackPopupMenu` call is then caught like everything else.

- **Antivirus and EDR false positives.** In-process DLL injection plus
  IAT patching plus `CoCreateInstance` Detours is exactly the signature
  pattern that EDR (Endpoint Detection & Response) tools watch for. Real
  risk of being flagged as malware. Code signing helps but doesn't fully
  solve it. **For now:** ship the Nilesoft-style approach because we know
  it works and the user base is small. **Later:** explore alternatives
  that aren't malware-pattern-shaped — possibilities include sparse
  packages with `IExplorerCommand` (Microsoft's blessed Win11 path,
  limited but not flagged), a UI Automation client that watches for
  context menus opening and overlays its own surface, or driver-level
  integration. Track this as a real follow-up, not a wishlist item.

- **Windows updates that change the menu path.** If Microsoft moves
  context-menu rendering off `TrackPopupMenu*` entirely (e.g., direct
  syscall, or fully XAML-rendered out-of-process), the IAT hooks become
  dead code overnight. Same risk for the modern-menu CLSID — if they
  change which COM class instantiates the menu, the `CoCreateInstance`
  hook stops catching it.

- **The modern menu may render out-of-process.** Some Win11 builds host
  parts of the modern shell in `sihost.exe` (Shell Infrastructure Host)
  or other broker processes, not in `explorer.exe`. For the file/folder/
  desktop right-click case Nilesoft handles via in-explorer hooks, the
  modern menu is actually instantiated through `CoCreateInstance`
  *inside* explorer (then XAML-rendered via XAML Islands in-process), so
  the in-explorer hook catches it. The cases where it might not: Start
  menu / Taskbar / Action Center context menus (different processes
  entirely), UWP packaged apps that show their own context menu, File
  Explorer windows running at higher integrity (admin-elevated).
  **Mitigations if it becomes a problem:** (a) inject our DLL into
  sihost.exe as well — same CLSID-registration mechanism would need a
  different entry point or a separate sparse-package handler. (b) use
  `SetWindowsHookEx` with a global WH_CALLWNDPROC hook that gets loaded
  into any process showing UI, then bootstrap our menu code from there.
  (c) accept the limitation and document that menus shown from broker
  processes fall back to the system default. We can decide which when we
  see the actual breakage.

- **Process integrity boundaries.** Explorer running at medium integrity
  cannot intercept menus shown by processes at higher integrity (UAC-
  elevated windows, system tray icons from system services). Our menu
  won't appear there.

- **Crashing explorer.exe.** Because the DLL lives inside explorer's
  process, any unhandled exception in our code crashes explorer. Two
  defenses, both required:

  1. **Wrap everything that touches the hook path in
     `__try/__except`** (SEH — Structured Exception Handling — catches
     access violations and other low-level faults that C++ `try/catch`
     does not). Every hook entry point (`TrackPopupMenuProc`,
     `TrackPopupMenuExProc`, the `CoCreateInstance` detour, the
     `LoadLibraryEx` hook) sits inside an `__try { ... } __except(...) `
     block. If anything throws or faults, we **fall back to calling the
     original function** with the original arguments. The user gets the
     normal Win11 menu instead of ours. That is acceptable degradation —
     they can right-click again and worst case they see Microsoft's
     menu, not a crashed shell.

  2. **Consumer click handlers run isolated.** When a consumer's
     `AddSelection(...) { ... }` body fires, we do not run that code on
     the explorer message-pump thread. Options: worker thread inside
     explorer (cheapest, still risks taking down explorer if the handler
     does something catastrophic like calling `ExitProcess`), or spawn a
     child process per click (safer, more overhead, breaks handlers that
     need to interact with explorer directly). Worker thread + `__try`
     wrapper is the MVP choice. Re-evaluate if real handlers start doing
     destabilizing things.

  The principle: explorer never crashes because of us. If our code
  cannot do its job, the system menu shows up instead.

- **DPI and multi-monitor.** Owner-drawn menus need to handle per-
  monitor DPI scaling, theme changes, RTL (right-to-left) languages, and
  high-contrast accessibility modes. Easy to get the common case right
  and ship something that looks broken on a 4K external monitor plugged
  into a 1080p laptop.

- **Unsigned DLL in explorer.** Exploit Guard, SmartScreen, and some
  enterprise policies block unsigned DLLs from loading into system
  processes. Distribution at any scale will need code signing.

- **Undocumented function dependency.** `NtUserTrackPopupMenuEx` is
  undocumented. Stable in practice but Microsoft has no obligation to
  keep it stable.


## Open Questions (need separate work)

These are the things this spec **does not yet decide**. They block writing
any code.

- **Packaging path detail.** Classic CLSID registration is confirmed
  as Nilesoft's path. We follow the same approach. Open detail: which
  asset types (file / folder / directory background / desktop / drive)
  we register against, and whether a single CLSID covers all of them or
  we register multiple.

- **Tech stack.** C++ with WIL (Windows Implementation Library) is the
  default for in-process shell extensions and matches Nilesoft's choice.
  Rust with the `windows` crate is viable. C# is discouraged for in-
  process shell extensions (Raymond Chen's longstanding guidance — needs
  reconfirmation for 2026). The pick affects how natural the consumer-
  facing API feels, so the stack decision is tied to the API ergonomics,
  not independent of them.

- **Consumer-language layer.** Since the menu definitions are Kotlin
  (or whichever final pick), and the shell extension is C++, we need a
  bridge — does the consumer compile their menu file into a side DLL
  the shell extension loads, or is there an embedded JVM/script host?
  Open question, important for the GUI editor downstream.

- **Handler isolation.** Consumer click handlers must not be able to
  crash explorer.exe. Run them on a worker thread? Spawn a child
  process per click? Both have tradeoffs.


## Future — GUI editor

Eventually there will be a GUI tool for editing menu definitions
visually — drag entries around, edit labels, attach handlers from
predefined templates, set conditions through dropdowns instead of
typing `if (target == File ...)`. Not in scope for the initial build.

The order of work is: raw code API first, prove it works end-to-end,
then build the GUI on top of it. The GUI will produce the same code
files a human would write by hand, so the two are interchangeable.


## Pending / WIP

Things this document does not cover yet and that should be added in
later passes:

- Architecture writeup (the answers to the Open Questions above).
- API surface document — exact method signatures in the chosen language,
  the context object's full shape, conditional visibility predicates
  (e.g., "only show for `.py` files"), separators, icons.
- Lifecycle: how the consumer's menu-definition code gets loaded by the
  shell-extension process. Is it compiled in? Loaded from a side DLL?
  Hot-reloadable?
- Error handling: what happens when a Selection's body throws or hangs.
  The shell process must not be brought down by a misbehaving handler.
- Persistence of definitions across explorer.exe restarts.
- Uninstall / disable path.
