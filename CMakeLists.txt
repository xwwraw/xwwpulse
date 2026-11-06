#  Same build as build.ps1, for Visual Studio / CMake users (MSVC x64, C++20).
cmake_minimum_required(VERSION 3.21)
project(xwwPulse LANGUAGES CXX RC)

file(READ version.txt XWW_VERSION_TEXT)
string(STRIP "${XWW_VERSION_TEXT}" XWW_VERSION_TEXT)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
add_compile_options(/W4 /permissive- /utf-8 /EHsc)
add_compile_definitions(UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX _WIN32_WINNT=0x0A00 XWW_VERSION=L"${XWW_VERSION_TEXT}")
include_directories(src res)

file(GLOB COMMON src/core/*.cpp src/config/*.cpp)
file(GLOB SERVICE src/detection/*.cpp src/optimizer/*.cpp src/service/*.cpp)
list(REMOVE_ITEM SERVICE ${CMAKE_CURRENT_SOURCE_DIR}/src/detection/foreground_hook.cpp)
file(GLOB DASHBOARD src/agent/*.cpp src/ui/*.cpp)
list(APPEND DASHBOARD src/detection/foreground_hook.cpp)
set_source_files_properties(res/service.rc res/dashboard.rc PROPERTIES COMPILE_DEFINITIONS "XWW_VERSION_TEXT=\"${XWW_VERSION_TEXT}\"")

add_executable(xwwPulseService ${COMMON} ${SERVICE} res/service.rc)
target_link_libraries(xwwPulseService PRIVATE advapi32 shell32 ole32 shlwapi ntdll powrprof tdh gdi32 version wtsapi32 userenv)

add_executable(xwwPulse WIN32 ${COMMON} ${DASHBOARD} res/dashboard.rc)
target_link_libraries(xwwPulse PRIVATE advapi32 shell32 ole32 shlwapi ntdll gdi32 dwmapi d2d1 dwrite windowscodecs uxtheme comctl32 version)
