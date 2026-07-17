cmake_minimum_required(VERSION 3.20)
project(peekbar VERSION 1.0.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

add_executable(peekbar WIN32
    src/main.cpp
    src/HostWindow.cpp
    src/WindowMonitor.cpp
    src/Renderer.cpp
    src/PaintUtil.cpp
    src/Store.cpp
    src/TabReader.cpp
    src/FanPopup.cpp
    src/Launcher.cpp
    src/ConfigWatcher.cpp
    src/TaskbarOverlayWindow.cpp
    src/Trace.cpp
)
# Pin SDK target: Win10 RS3+. Need >= RS2 for DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2.
target_compile_options(peekbar PRIVATE /utf-8)
target_compile_definitions(peekbar PRIVATE
    UNICODE _UNICODE
    _WIN32_WINNT=0x0A00
    WINVER=0x0A00
    NTDDI_VERSION=0x0A000004
)
target_link_libraries(peekbar PRIVATE shell32 user32 gdi32 msimg32 ole32 oleaut32 UIAutomationCore dwmapi)

# Decoupled observability tool: separate target, no shared code, never linked
# into the shell. One repo, two targets — see docs/profiler-project-structure.md.
add_subdirectory(profiler)

# --- User distributable (CPack ZIP) ------------------------------------------
# Only peekbar has install() rules, so the ZIP ships the shell exe + docs only;
# shell_profiler is never bundled (hard rule 8).
install(TARGETS peekbar RUNTIME DESTINATION .)
install(FILES
    packaging/README.txt
    packaging/config.sample.txt
    packaging/install.ps1
    packaging/uninstall.ps1
    DESTINATION .
)
set(CPACK_GENERATOR "ZIP")
set(CPACK_PACKAGE_NAME "Peekbar")
set(CPACK_PACKAGE_VENDOR "Peekbar")
set(CPACK_PACKAGE_VERSION "${PROJECT_VERSION}")
set(CPACK_PACKAGE_FILE_NAME "Peekbar-${PROJECT_VERSION}-win64")
set(CPACK_PACKAGE_DIRECTORY "${CMAKE_BINARY_DIR}/dist")
include(CPack)
