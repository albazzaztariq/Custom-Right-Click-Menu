@echo off
REM Reverses register.bat. Removes all HKCR / HKLM keys written by
REM DllRegisterServer. Restart explorer afterwards to detach.

setlocal
set DLL=%~dp0build\shell_extension.dll
if not exist "%DLL%" (
    echo Build artifact missing: %DLL%
    echo Nothing to unregister (or already removed).
    exit /b 0
)

echo Unregistering %DLL% ...
regsvr32 /s /u "%DLL%"
if errorlevel 1 (
    echo regsvr32 /u failed.
    exit /b 1
)
echo Done. Restart explorer.exe to detach the hooks.
