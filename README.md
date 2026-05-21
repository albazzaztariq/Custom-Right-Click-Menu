# Right-Click Menu API

Windows 11 shell extension that replaces (not extends) the right-click
context menu. Consumer writes a `menu.cpp` file that describes the menu
in a declarative tree; our DLL intercepts explorer.exe's menu code path
and shows the consumer's menu instead.

See `Rough Spec.md` for the full design, the architecture (verified
against Nilesoft Shell's source), and the risk list.


## Build

From a **x64 Native Tools Command Prompt for VS 2022**:

```
build.bat
```

Outputs to `build\`:

| Artifact              | What it is                                                 |
|-----------------------|------------------------------------------------------------|
| `test_harness.exe`    | Standalone exe. Calls the API directly. No hook involved. |
| `shell_extension.dll` | The host DLL — IAT hooks + owner-drawn menu window.       |
| `menu.dll`            | Sample consumer menu, compiled from `examples/menu.cpp`.  |
| `hook_test.exe`       | Loads `shell_extension.dll`, fires a `TrackPopupMenuEx` call. Proves the hook path works without explorer.exe. |


## Iteration plan

The project ships in layers, each independently verifiable, so an
explorer.exe crash from a bug in late-stage code never happens because
earlier stages caught it.

**Stage 1 — API works (DONE).**
Run `build\test_harness.exe`. A menu pops up at the cursor. Click an
item, its handler runs (a `MessageBox`). The API surface, nested
submenus, conditional entries, and click dispatch are all proven.

**Stage 2 — Hook path works (DONE, ready to test).**
Run `build\hook_test.exe`. It `LoadLibrary`s `shell_extension.dll`,
which installs IAT hooks on `TrackPopupMenu` / `TrackPopupMenuEx` in
its own process. It then calls `TrackPopupMenuEx`. The IAT hook fires,
our detour runs, builds the menu from `menu.dll`'s `RegisterMenu()`,
and shows our window — *not* a system menu. Proves the hook pipeline
works end-to-end without touching explorer.

**Stage 3 — Explorer integration (NOT YET).**
Register `shell_extension.dll` as a CLSID under `HKCR\CLSID\{...}`
with appropriate asset-type associations. Kill explorer. New explorer
loads the DLL. IAT hooks now sit inside explorer. Right-clicks in
File Explorer trigger our menu. Real-world test.

**Stage 4 — Real context extraction.**
Currently the hook hardcodes `target = DirectoryBackground`. Stage 4
queries `IShellView` / `IFolderView` to figure out which files were
clicked, populates the context globals correctly, so the consumer's
conditional entries (`if (target == ClickTarget::File && ...)`)
actually fire on real selections.

**Stage 5 — Modern menu suppression.**
Wire up the Microsoft Detours hook on `CoCreateInstance` so the Win11
modern menu doesn't appear (currently it might still come up alongside
ours, depending on the code path).

**Stage 6+ — see `Rough Spec.md` "Pending / WIP".**


## Project layout

```
Right-Click Menu API/
├── Rough Spec.md           — design + architecture + risks
├── README.md               — this file
├── CMakeLists.txt          — CMake build (alternative to build.bat)
├── build.bat               — one-shot cl.exe build (recommended)
├── src/
│   ├── menu_api.h          — consumer-facing header (the only file
│   │                         a consumer's menu.cpp needs to include)
│   ├── menu_api.cpp        — macro helpers + tree builder + registry
│   ├── menu_node.h         — MenuNode + handler-registry decls
│   ├── context.h           — host-side context helpers
│   ├── context.cpp         — context globals + extension parsing
│   ├── main.cpp            — DllMain + hook installation
│   ├── hook.h / .cpp       — IAT hook class + TrackPopupMenu detours
│   ├── menu_window.h / .cpp — owner-drawn menu surface
│   └── detours_hook.h / .cpp — CoCreateInstance hook (stubbed)
├── examples/
│   └── menu.cpp            — sample consumer file
├── test_harness/
│   └── test_harness.cpp    — direct-call test exe
└── hook_test/
    └── hook_test.cpp       — hook-path test exe
```
