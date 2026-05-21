@echo off
REM Registers shell_extension.dll as a Windows context-menu handler.
REM Requires admin rights — regsvr32 will UAC-prompt if not elevated.
REM
REM What this does:
REM   - Writes HKCR\CLSID\{7C3F4C5E-9D2E-4F7A-B5C8-1A3D4E5F6A7B} pointing
REM     at build\shell_extension.dll (InProcServer32 = full path).
REM   - Registers the handler under * / Directory / Directory\Background
REM     / Drive / Folder / AllFilesystemObjects shell scopes.
REM   - Adds the CLSID to HKLM\...\Shell Extensions\Approved.
REM
REM After running this:
REM   1) Kill explorer.exe in Task Manager.
REM   2) Start it again (Win+E, or File > Run new task > explorer.exe).
REM   3) New explorer.exe loads shell_extension.dll on its first
REM      right-click, which installs the TrackPopupMenuEx hooks, and
REM      our menu replaces the system one.
REM
REM To remove: run unregister.bat.

setlocal
set DLL=%~dp0build\shell_extension.dll
if not exist "%DLL%" (
    echo Build first: build.bat
    echo Missing: %DLL%
    exit /b 1
)

echo Registering %DLL% ...
regsvr32 /s "%DLL%"
if errorlevel 1 (
    echo regsvr32 failed.
    exit /b 1
)
echo Done. Restart explorer.exe to load the hooks.
