@echo off
setlocal enabledelayedexpansion

if "%~1"=="" exit /b 1
set "src=%~1"
set "leaf=%~nx1"
set "leaf_noext=%~n1"

set "temp_vault=C:\Users\azt12\Temp Obsidian Vault"
set "temp_file=%temp_vault%\%leaf%"

mkdir "%temp_vault%" 2>nul
mkdir "%temp_vault%\.obsidian" 2>nul

if exist "%temp_file%" (
    set "n=2"
    :find_unique
    set "temp_file=%temp_vault%\%leaf_noext% (!n!).md"
    if exist "!temp_file!" (set /a n+=1 & goto find_unique)
)

copy /Y "%src%" "!temp_file!" >nul 2>&1

rem Recompute leaf + leaf_noext from the actual on-disk path. The
rem dedup branch above can change temp_file to "Foo (2).md"; the URL
rem and the watcher both need the final name, not the original.
for %%I in ("!temp_file!") do (
    set "final_leaf=%%~nxI"
    set "final_leaf_noext=%%~nI"
)

rem Open via obsidian:// URL. The C++ shell-extension version of this
rem flow works because it URL-encodes BOTH vault and file via
rem ShellExecuteA. Every batch attempt that handed an un-encoded URL
rem (literal spaces in vault and file) failed the same way: Obsidian
rem opens the vault but parses the file= value only up to the first
rem space, so the lookup misses and the window shows a blank "New
rem tab". Fix: encode both components with .NET's EscapeDataString
rem (RFC 3986 unreserved set — matches the C++ UrlEncode) and let
rem Start-Process ShellExecute the result. The leaf is passed via env
rem var so we never have to fight cmd quoting for it.
set "_OBS_LEAF=!final_leaf!"
echo DEBUG: final_leaf=!final_leaf! > "C:\Users\azt12\open_md_debug.log"
powershell -NoProfile -WindowStyle Hidden -Command "$vault = [uri]::EscapeDataString('Temp Obsidian Vault'); $file = [uri]::EscapeDataString($env:_OBS_LEAF); $url = 'obsidian://open?vault=' + $vault + '&file=' + $file; echo DEBUG URL: $url >> 'C:\Users\azt12\open_md_debug.log'; Start-Process $url"
set "_OBS_LEAF="

rem Spawn the watcher with the FINAL leaf-no-ext so its title match
rem ("<leaf> - Temp Obsidian Vault - Obsidian") lines up with the
rem actual Obsidian window — and with the FINAL temp_file path so it
rem deletes the file it really has.
set "watcher=%~dp0obs_watcher.exe"
if exist "!watcher!" start /B "" "!watcher!" "!temp_file!" "!final_leaf_noext!" "Temp Obsidian Vault"

exit /b 0