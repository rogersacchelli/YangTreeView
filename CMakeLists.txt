# Visual Studio 2019+ (static CRT - no VC++ redistributable needed):
#   cmake -B build -A x64     && cmake --build build --config Release
#   cmake -B build32 -A Win32 && cmake --build build32 --config Release
#   cmake -B buildarm -A ARM64 && cmake --build buildarm --config Release
cmake_minimum_required(VERSION 3.15)
project(YangTreeView CXX RC)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
add_compile_definitions(UNICODE _UNICODE _CRT_SECURE_NO_WARNINGS NOMINMAX)
if(MSVC)
  add_compile_options(/utf-8 /W3 /permissive-)
endif()

# Notepad++ plugin
add_library(YangTreeView SHARED src/plugin.cpp src/yang_core.cpp src/version.rc)
target_link_libraries(YangTreeView PRIVATE user32 shell32)

# Command-line tool (same engine)
add_executable(yang2html src/cli_main.cpp src/yang_core.cpp)
target_link_libraries(yang2html PRIVATE shell32)

# Fake Notepad++ host used by the self-test
add_executable(fakehost tests/fakehost.cpp)
target_link_libraries(fakehost PRIVATE user32)
