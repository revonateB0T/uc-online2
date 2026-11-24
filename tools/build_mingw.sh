#!/usr/bin/env bash
# ============================================================
# Cross-build the Windows DLL (steam_api64.dll) with MinGW-w64.
#
# The MSVC build needs Visual Studio; this script produces the same
# DLL from Linux, for machines that have no MSVC.  The result is
# fully static: it links the C++ runtime into the DLL, so it does
# not need libstdc++-6.dll / libgcc_s_seh-1.dll / libwinpthread-1.dll
# next to it.  The only imports left are the Windows system DLLs
# (kernel32 and friends).
#
#   tools/build_mingw.sh [output-dir]
#
# Needs: x86_64-w64-mingw32-g++ (mingw-w64), and the MinHook static
# library in lib/libMinHook.x64.a.  To build that library from the
# upstream sources instead:
#
#   curl -sSL -o /tmp/minhook.tar.gz \
#     https://github.com/TsudaKageyu/minhook/archive/refs/heads/master.tar.gz
#   mkdir -p /tmp/minhook && tar xzf /tmp/minhook.tar.gz -C /tmp/minhook
#   cd /tmp/minhook/minhook-master
#   x86_64-w64-mingw32-gcc -c -O2 -masm=intel -std=c11 -Iinclude -Isrc \
#     src/*.c src/hde/*.c
#   x86_64-w64-mingw32-ar rcs lib/libMinHook.x64.a *.o
# ============================================================
set -euo pipefail

here="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$here"

out="${1:-$here/build/mingw}"
mkdir -p "$out"

CXX="${CXX:-x86_64-w64-mingw32-g++}"
WINDRES="${WINDRES:-x86_64-w64-mingw32-windres}"
MH="$here/lib/libMinHook.x64.a"

command -v "$CXX" >/dev/null || { echo "missing $CXX -- install mingw-w64" >&2; exit 1; }
[ -f "$MH" ] || { echo "missing $MH -- see the header of this script" >&2; exit 1; }

echo "== compiling dllmain.cpp"
"$CXX" -c -O2 -std=c++17 -fno-strict-aliasing -I"$here" \
    -o "$out/dllmain.o" "$here/dllmain.cpp"

echo "== compiling the resources"
"$WINDRES" -I "$here" -DIS_64BIT -i "$here/uc_online2.rc" -o "$out/uc_online2_rc.o"

echo "== linking steam_api64.dll"
# -static pulls in the C++ runtime and the threading library, so the
# DLL has no dependency on any other DLL but the Windows ones.
"$CXX" -shared -o "$out/steam_api64.dll" \
    "$out/dllmain.o" "$out/uc_online2_rc.o" \
    "$MH" -lshlwapi \
    -static -static-libgcc -static-libstdc++ \
    -Wl,--enable-stdcall-fixup \
    -Wl,--out-implib,"$out/libsteam_api64.a"

echo
echo "== imports"
x86_64-w64-mingw32-objdump -p "$out/steam_api64.dll" \
    | grep "DLL Name" | sort -u

echo
echo "== exports"
x86_64-w64-mingw32-objdump -p "$out/steam_api64.dll" \
    | grep -c "SteamAPI_" | xargs -I{} echo "  {} SteamAPI_* exports"

ls -la "$out/steam_api64.dll"
