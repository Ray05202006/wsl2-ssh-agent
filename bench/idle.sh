#!/bin/bash
# Long idle test: start the original and the new agent, do one request each,
# then sample CPU time / context switches (Linux) and CPU time / memory
# (Windows) only at the start and end of the idle window.
#   bench/idle.sh ORIG_BIN NEW_BIN SECONDS
set -u
here=$(cd "$(dirname "$0")" && pwd); R=${XDG_RUNTIME_DIR:-/tmp}/wsa-idle; mkdir -p "$R"
winmem() { (cd /tmp && powershell.exe -NoProfile -ExecutionPolicy Bypass -File "$(wslpath -w "$here/winmem.ps1")" "$@" | tr -d '\r'); }
pre=$(winmem powershell.exe | sed -n 's/.* pid=\([0-9]*\) .*/\1/p' | paste -sd,)
"$1" -foreground -socket $R/o.sock >/dev/null 2>&1 & O=$!
"$2" -foreground -socket $R/n.sock >/dev/null 2>&1 & N=$!
for s in o n; do until SSH_AUTH_SOCK=$R/$s.sock ssh-add -l >/dev/null 2>&1; do sleep 0.05; done; done
sleep 5
snap() { echo "== $1"; "$here/linmem.sh" $O; "$here/linmem.sh" $N; winmem powershell.exe "$pre"; }
snap start; sleep "$3"; snap "end (+$3 s)"
kill $O $N
