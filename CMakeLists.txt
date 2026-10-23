cmake_minimum_required(VERSION 3.20)

file(STRINGS "${CMAKE_CURRENT_SOURCE_DIR}/VERSION" OFWM_VERSION LIMIT_COUNT 1)
project(ObjectFrame VERSION ${OFWM_VERSION} LANGUAGES CXX RC)
configure_file(src/version.h.in "${CMAKE_CURRENT_BINARY_DIR}/generated/version.h" @ONLY)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

add_executable(ofwm WIN32
    src/main.cpp
    src/common.cpp
    src/config.cpp
    src/layout.cpp
    src/window_utils.cpp
    src/window_manager.cpp
    src/keyboard_hook.cpp
    src/settings_window.cpp
    src/border_overlay.cpp
    src/animator.cpp
    src/app.manifest
    src/ofwm.rc
)

target_compile_definitions(ofwm PRIVATE UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX)
target_include_directories(ofwm PRIVATE "${CMAKE_CURRENT_BINARY_DIR}/generated")

if(MSVC)
    target_compile_options(ofwm PRIVATE /W4 /utf-8 /permissive-)
    set_property(TARGET ofwm PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
endif()

target_link_libraries(ofwm PRIVATE dwmapi shell32 user32 advapi32 comctl32 comdlg32 uxtheme shcore winmm gdiplus)
