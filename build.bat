@echo off
REM Builds all artifacts. Run from a VS Developer Command Prompt.

setlocal
where cl >nul 2>nul
if errorlevel 1 (
    echo cl.exe not on PATH. Open x64 Native Tools Command Prompt.
    exit /b 1
)

if not exist build mkdir build
if not exist build\obj mkdir build\obj
if not exist build\assets mkdir build\assets
if not exist build\assets\icons mkdir build\assets\icons

REM Copy icon PNGs next to the DLL so the runtime resolver finds them.
REM Robocopy exit code <= 7 is success.
robocopy assets\icons build\assets\icons /MIR /NJH /NJS /NDL /NP >nul
if errorlevel 8 (
    echo Failed to copy assets\icons -> build\assets\icons
    exit /b 1
)

REM Copy the variable font next to the DLL. DirectWrite reads the
REM whole wght axis from it (no static-weight TTFs needed).
if not exist build\Font mkdir build\Font
robocopy Font build\Font Exo2-VariableFont_wght.ttf /NJH /NJS /NDL /NP >nul
if errorlevel 8 (
    echo Failed to copy Font\Exo2-VariableFont_wght.ttf -> build\Font
    exit /b 1
)

set COMMON=/nologo /EHsc /std:c++17 /W3 /utf-8 /DUNICODE /D_UNICODE /DWIN32_LEAN_AND_MEAN /DNOMINMAX

echo === Building test_harness.exe (static) ===
cl %COMMON% /DMENU_API_STATIC /Fe:build\test_harness.exe /Fo:build\obj\ ^
   src\menu_api.cpp src\context.cpp src\menu_window.cpp src\diag.cpp ^
   test_harness\test_harness.cpp ^
   user32.lib gdi32.lib ole32.lib windowscodecs.lib msimg32.lib ^
   d2d1.lib dwrite.lib gdiplus.lib
if errorlevel 1 goto fail

echo.
echo === Building shell_extension.dll (host, exports API) ===
cl %COMMON% /DMENU_API_BUILDING_DLL /LD /Fe:build\shell_extension.dll /Fo:build\obj\ ^
   src\main.cpp src\menu_api.cpp src\context.cpp src\menu_window.cpp ^
   src\hook.cpp src\detours_hook.cpp src\shell_context.cpp ^
   src\folder_view_actions.cpp ^
   src\class_factory.cpp src\diag.cpp ^
   user32.lib gdi32.lib psapi.lib ole32.lib oleaut32.lib shell32.lib shlwapi.lib advapi32.lib ^
   d2d1.lib dwrite.lib gdiplus.lib windowscodecs.lib msimg32.lib ^
   /link /DEF:src\shell_extension.def
if errorlevel 1 goto fail

echo.
echo === Building menu.dll (consumer, imports API from shell_extension) ===
cl %COMMON% /LD /Fe:build\menu.dll /Fo:build\obj\ ^
   examples\menu.cpp ^
   build\shell_extension.lib user32.lib shell32.lib ole32.lib oleaut32.lib oleacc.lib
if errorlevel 1 goto fail

echo.
echo === Building hook_test.exe ===
cl %COMMON% /Fe:build\hook_test.exe /Fo:build\obj\ ^
   hook_test\hook_test.cpp ^
   user32.lib
if errorlevel 1 goto fail

echo.
echo All builds OK. Artifacts:
dir /B build\*.exe build\*.dll
goto :eof

:fail
echo.
echo Build failed.
exit /b 1
