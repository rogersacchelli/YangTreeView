#!/usr/bin/env bash
# Cross-build with MinGW-w64 (Linux/WSL/MSYS2). Produces fully static binaries:
#   dist/x64/YangTreeView/YangTreeView.dll   dist/x64/yang2html.exe
#   dist/x86/YangTreeView/YangTreeView.dll   dist/x86/yang2html.exe
set -euo pipefail
cd "$(dirname "$0")"
python3 tools/gen_template.py >/dev/null 2>&1 || echo "note: python3 not found, using existing src/template_html.h"
FLAGS="-std=c++17 -DNOMINMAX -O2 -Wall -Wextra -DUNICODE -D_UNICODE -static -static-libgcc -static-libstdc++ -s"
for arch in x64 x86; do
  if [ $arch = x64 ]; then T=x86_64-w64-mingw32; else T=i686-w64-mingw32; fi
  out=dist/$arch; mkdir -p $out/YangTreeView
  $T-windres src/version.rc -O coff -o /tmp/ver_$arch.o
  $T-g++ $FLAGS -shared -o $out/YangTreeView/YangTreeView.dll \
      src/plugin.cpp src/yang_core.cpp /tmp/ver_$arch.o -luser32 -lshell32
  $T-g++ $FLAGS -o $out/yang2html.exe src/cli_main.cpp src/yang_core.cpp -lshell32
  echo "built $arch"
done
