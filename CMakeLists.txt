# Visual Studio 2019+ (static CRT - no VC++ redistributable needed):
#   cmake -B build -A x64     && cmake --build build --config Release
#   cmake -B build32 -A Win32 && cmake --build build32 --config Release
#   cmake -B buildarm -A ARM64 && cmake --build buildarm --config Release
# The WebView2 SDK (docked viewer) is downloaded from nuget.org at configure time;
# for offline builds pass -DWEBVIEW2_DIR=<extracted Microsoft.Web.WebView2 package>.
cmake_minimum_required(VERSION 3.18)
project(YangTreeView CXX RC)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
add_compile_definitions(UNICODE _UNICODE _CRT_SECURE_NO_WARNINGS NOMINMAX)
if(MSVC)
  add_compile_options(/utf-8 /W3 /permissive-)
endif()

# --- WebView2 SDK -----------------------------------------------------------
set(WEBVIEW2_VERSION 1.0.4314.35)
set(WEBVIEW2_SHA256 90778deb1b77e36292828b4b5833c9255c27533f40e0ddd55891ae718944edc4)
if(NOT WEBVIEW2_DIR)
  set(WEBVIEW2_DIR ${CMAKE_BINARY_DIR}/_deps/webview2-${WEBVIEW2_VERSION})
  if(NOT EXISTS ${WEBVIEW2_DIR}/build/native/include/WebView2.h)
    set(_pkg ${CMAKE_BINARY_DIR}/_deps/webview2-${WEBVIEW2_VERSION}.nupkg)
    message(STATUS "Downloading Microsoft.Web.WebView2 ${WEBVIEW2_VERSION}")
    file(DOWNLOAD https://www.nuget.org/api/v2/package/Microsoft.Web.WebView2/${WEBVIEW2_VERSION} ${_pkg}
         EXPECTED_HASH SHA256=${WEBVIEW2_SHA256} TLS_VERIFY ON)
    file(ARCHIVE_EXTRACT INPUT ${_pkg} DESTINATION ${WEBVIEW2_DIR})
  endif()
endif()
if(CMAKE_GENERATOR_PLATFORM MATCHES "^[Ww]in32$" OR (NOT CMAKE_GENERATOR_PLATFORM AND CMAKE_SIZEOF_VOID_P EQUAL 4))
  set(_wv2arch x86)
elseif(CMAKE_GENERATOR_PLATFORM MATCHES "^ARM64$")
  set(_wv2arch arm64)
else()
  set(_wv2arch x64)
endif()

# Notepad++ plugin
add_library(YangTreeView SHARED src/plugin.cpp src/dock_panel.cpp src/yang_core.cpp src/version.rc)
target_include_directories(YangTreeView PRIVATE ${WEBVIEW2_DIR}/build/native/include)
target_compile_definitions(YangTreeView PRIVATE YTV_WEBVIEW2_STATIC)
target_link_libraries(YangTreeView PRIVATE user32 gdi32 shell32 ole32 comctl32 advapi32
                      ${WEBVIEW2_DIR}/build/native/${_wv2arch}/WebView2LoaderStatic.lib)

# Command-line tool (same engine)
add_executable(yang2html src/cli_main.cpp src/yang_core.cpp)
target_link_libraries(yang2html PRIVATE shell32)

# Fake Notepad++ host used by the self-test
add_executable(fakehost tests/fakehost.cpp)
target_link_libraries(fakehost PRIVATE user32)
