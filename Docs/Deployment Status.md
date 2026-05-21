# Right-Click Menu API — Deployment Status

_Last updated: 2026-05-16_

## Where we are

Stages 1-2 (hook_test.exe and test_harness.exe): **working**. Menu renders correctly with icons + text + separators + shortcuts.

Stage 3 live in explorer: **working**. Our menu replaces the native Win11 24H2 modern menu on right-click in File Explorer.

## Confirmed facts

- DLL is registered correctly: `HKCR\CLSID\{7C3F4C5E-9D2E-4F7A-B5C8-1A3D4E5F6A7B}\InProcServer32` points at `WORKING\build\shell_extension.dll`.
- All asset associations registered: `*`, `Directory`, `Directory\Background`, `Drive`, `Folder`, `AllFilesystemObjects`.
- `HKLM\Software\Microsoft\Windows\CurrentVersion\Shell Extensions\Approved` has our CLSID.
- DLL **does** load into explorer.exe (verified via `tasklist /m shell_extension.dll`).
- `install_iat_hooks` reports successful patches. Inline hooks on `user32!TrackPopupMenuEx`, `user32!TrackPopupMenu`, and `win32u!NtUserTrackPopupMenuEx` installed.
- HKCU `{86ca1aa0-34aa-4e8b-a509-50c905bae2a2}\InprocServer32 = ""` — the well-known Win10-style menu trick.
- HKLM mirror of the same override — added 2026-05-14.

## Environment notes

- Windows 11 24H2, OS build 10.0.26200.8246 (Insider).
- ExplorerPatcher is loaded into explorer (`ep_taskbar.5.dll` plus `[SMA]`, `[JVP]`, `[TB]`, `[IME]` patcher logs visible in the explorer launch output).
- EP root settings: `OldTaskbar = 2`, `FileExplorerCommandUI = 0`, `MigratedFromOldSettings = 1`. No obvious context-menu-related EP toggle.

## Top hypotheses (from three independent investigations: Claude Code subagent, ask-claude, ask-grok)

All three independently converged on these:

1. **Win11 24H2 modern menu doesn't go through `TrackPopupMenuEx` at all** (cause confidence: 75-80%). The menu is composed by XAML/Islands. Our hook target is wrong for this OS.
2. **The `{86ca1aa0-...}` HKCU override may be neutralized on 24H2 build 26200** (cause confidence: 65-75%). Worked on 22H2/23H2; possibly broken on 24H2.
3. **ExplorerPatcher patches the menu pipeline itself** (cause confidence: 55-80%), short-circuiting third-party shellex handler enumeration.

## Fixes applied 2026-05-14

- Added `win32u.dll!NtUserTrackPopupMenuEx` to both IAT and inline-patch hook sets.
- Added `user32!TrackPopupMenu` (non-Ex) to inline-patch hook set (was only Ex before).
- Mirrored the `{86ca1aa0-...}\InprocServer32` empty default to HKLM in addition to HKCU.

## What to try next (in order)

1. **Normal right-click** after explorer restart. If our menu fires → HKLM mirror was missing piece.
2. **Shift+right-click** → forces legacy menu. If our menu fires only here → modern menu disable still failing; need a different CLSID or method.
3. **Check `build\shell_extension.log`** for `FIRED` / `NtUserTrackPopupMenuEx` lines. Their presence determines which path explorer actually takes.
4. **Temporarily disable ExplorerPatcher**: rename `ep_taskbar.5.dll` (anywhere under EP's install dir, search for it) → restart explorer. If our menu fires now → EP is overriding.
5. **If still no fire**: the modern menu doesn't go through any TPM entry point. Need to either (a) implement `IExplorerCommand` with proper `CommandStore\Shell\` registration, or (b) hook the XAML menu construction (much harder), or (c) use ViveTool to disable the modern-menu feature flag.

## Rollback / cleanup

```
WORKING\unregister.bat   # admin, removes HKCR registrations
```

Then delete:
- `HKCU\Software\Classes\CLSID\{86ca1aa0-34aa-4e8b-a509-50c905bae2a2}`
- `HKLM\Software\Classes\CLSID\{86ca1aa0-34aa-4e8b-a509-50c905bae2a2}`

Then restart explorer.

## Files touched this session

- `WORKING\src\hook.cpp` — added `NtUserTrackPopupMenuEx` IAT + inline hooks; added `TrackPopupMenu` inline hook; added `g_inline_TPM`, `g_inline_NTPMEX` state.
- `Rewind1\` — refreshed to match `WORKING\` at the moment text rendering started working (label weight 450 / shortcut+chevron 250).
