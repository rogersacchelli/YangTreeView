#!/usr/bin/env bash
# Cross-build with MinGW-w64 (Linux/WSL/MSYS2). Produces fully static binaries:
#   dist/x64/YangTreeView/YangTreeView.dll   dist/x64/yang2html.exe
#   dist/x86/YangTreeView/YangTreeView.dll   dist/x86/yang2html.exe
# The docked viewer needs the WebView2 SDK (downloaded from nuget.org into build-mingw/).
# WebView2LoaderStatic.lib is MSVC-only, so MinGW builds load WebView2Loader.dll at
# runtime; it is copied next to YangTreeView.dll and must be shipped with it.
# (Release zips are built by CI with MSVC and contain only YangTreeView.dll.)
set -euo pipefail
cd "$(dirname "$0")"
python3 tools/gen_template.py >/dev/null 2>&1 || echo "note: python3 not found, using existing src/template_html.h"

WV2_VERSION=1.0.4314.35   # keep in sync with CMakeLists.txt
WV2_SHA256=90778deb1b77e36292828b4b5833c9255c27533f40e0ddd55891ae718944edc4
WV2=build-mingw/webview2-$WV2_VERSION
if [ ! -f $WV2/build/native/include/WebView2.h ]; then
  mkdir -p build-mingw
  curl -fsSL -o build-mingw/webview2.nupkg "https://www.nuget.org/api/v2/package/Microsoft.Web.WebView2/$WV2_VERSION"
  echo "$WV2_SHA256  build-mingw/webview2.nupkg" | sha256sum -c -
  rm -rf $WV2 && mkdir -p $WV2 && (cd $WV2 && unzip -q ../webview2.nupkg 'build/native/*')
fi
# WebView2.h includes "EventToken.h"; MinGW ships it as eventtoken.h (include_next also
# works where the file system is case-insensitive and the shim finds itself first)
mkdir -p build-mingw/shim && printf '#pragma once\n#include_next <eventtoken.h>\n' > build-mingw/shim/EventToken.h

FLAGS="-std=c++17 -DNOMINMAX -O2 -Wall -Wextra -DUNICODE -D_UNICODE -static -static-libgcc -static-libstdc++ -s"
for arch in x64 x86; do
  if [ $arch = x64 ]; then T=x86_64-w64-mingw32; else T=i686-w64-mingw32; fi
  out=dist/$arch; mkdir -p $out/YangTreeView
  $T-windres src/version.rc -O coff -o build-mingw/ver_$arch.o
  $T-g++ $FLAGS -Wno-unknown-pragmas -Wno-unused-parameter -I build-mingw/shim -I $WV2/build/native/include \
      -shared -o $out/YangTreeView/YangTreeView.dll \
      src/plugin.cpp src/dock_panel.cpp src/yang_core.cpp build-mingw/ver_$arch.o \
      -luser32 -lgdi32 -lshell32 -lole32 -lcomctl32 -ladvapi32 -luuid
  cp $WV2/build/native/$arch/WebView2Loader.dll $out/YangTreeView/
  $T-g++ $FLAGS -o $out/yang2html.exe src/cli_main.cpp src/yang_core.cpp -lshell32
  echo "built $arch"
done
