cmake_minimum_required(VERSION 3.15)
project(RightClickMenuAPI CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if(MSVC)
    add_compile_options(/utf-8 /W3)
endif()

# ─── Test harness ────────────────────────────────────────────────────
# Standalone .exe that pops up the menu directly. No shell extension,
# no explorer.exe, no CLSID. Just builds and runs.
add_executable(test_harness
    src/menu_api.cpp
    src/context.cpp
    src/menu_window.cpp
    test_harness/test_harness.cpp
)
target_include_directories(test_harness PRIVATE src)
target_link_libraries(test_harness PRIVATE user32 gdi32)
target_compile_definitions(test_harness PRIVATE
    UNICODE _UNICODE
    WIN32_LEAN_AND_MEAN
    NOMINMAX
)
