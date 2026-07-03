cmake_minimum_required(VERSION 3.20)
project(browser_shell_os LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

add_executable(browser_shell_os WIN32
    src/main.cpp
    src/DockWindow.cpp
    src/WindowMonitor.cpp
)
# Pin SDK target: Win10 RS3+. Need >= RS2 for DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2.
target_compile_definitions(browser_shell_os PRIVATE
    UNICODE _UNICODE
    _WIN32_WINNT=0x0A00
    WINVER=0x0A00
    NTDDI_VERSION=0x0A000004
)
target_link_libraries(browser_shell_os PRIVATE shell32 user32 gdi32 psapi)
