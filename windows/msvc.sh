#!/bin/bash
# Build a Windows C source with the host's MSVC (via WSL interop).
# usage: msvc.sh SRC.c OUT.exe [extra cl/link flags...]
# Env: VCVARS (path to vcvars64.bat, Windows path) may override autodetection.
set -euo pipefail
src=$1 out=$2; shift 2
vcvars=${VCVARS:-}
if [ -z "$vcvars" ]; then
  for f in "/mnt/c/Program Files/Microsoft Visual Studio/"*/*/VC/Auxiliary/Build/vcvars64.bat \
           "/mnt/c/Program Files (x86)/Microsoft Visual Studio/"*/*/VC/Auxiliary/Build/vcvars64.bat; do
    [ -f "$f" ] && vcvars=$(wslpath -w "$f") && break
  done
fi
[ -n "$vcvars" ] || { echo "vcvars64.bat not found; set VCVARS" >&2; exit 1; }
tmp=$(wslpath "$(cmd.exe /c 'echo %TEMP%' 2>/dev/null | tr -d '\r')")/wsa-build.$$
mkdir -p "$tmp"; trap 'rm -rf "$tmp"' EXIT
cp "$src" "$tmp/src.c"
# /O1 size, /GS- no security cookie (no CRT), /Gs999999 no stack probes, /NODEFAULTLIB no CRT
# cmd.exe quoting through interop is fragile; go through a .bat file instead
cat > "$tmp/build.bat" <<BAT
@echo off
call "$vcvars" >nul || exit /b 1
cl /nologo /O1 /Os /GS- /Gs999999 /W3 /c src.c $* || exit /b 1
link /nologo /NODEFAULTLIB /ENTRY:start /SUBSYSTEM:CONSOLE /OPT:REF /OPT:ICF /MERGE:.rdata=.text /DEBUG:NONE src.obj kernel32.lib /OUT:out.exe || exit /b 1
BAT
(cd "$tmp" && cmd.exe /c build.bat 2>&1 | tr -d '\r' | grep -v '^src.c$' || true)
[ -f "$tmp/out.exe" ] || { echo "build failed" >&2; exit 1; }
cp "$tmp/out.exe" "$out"; chmod +x "$out"
