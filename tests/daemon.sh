#!/bin/bash
# Daemon-only tests that run on any Linux box (CI): the Windows helper is
# replaced by `cat`, so a connection simply echoes what the client sends.
# This exercises everything the Linux side does: CLI, daemonizing, socket
# permissions, fd hand-off to the helper, concurrency, reaping, stop/restart.
#
#   tests/daemon.sh [path/to/wsl2-ssh-agent]
set -u
cd "$(dirname "$0")/.."
BIN=$(realpath "${1:-./wsl2-ssh-agent}")
grep -qa __asan_init "$BIN" && SANITIZED=1 || SANITIZED=0
T=$(mktemp -d "${XDG_RUNTIME_DIR:-/tmp}/wsa-dtest.XXXXXX")
SOCK=$T/s.sock
mkdir -p build && cc -O2 -o build/sockcat tests/sockcat.c || exit 1
SC=$PWD/build/sockcat
printf '#!/bin/sh\necho "helper args: $*" >&2\nexec cat\n' > "$T/echo-helper"; chmod 700 "$T/echo-helper"
A=(-socket "$SOCK" -helper "$T/echo-helper" -log "$T/log")
pass=0 fail=0
gone() { [ ! -e /proc/$1 ] || case "$(awk '{print $3}' /proc/$1/stat 2>/dev/null)" in Z|X|"") true;; *) false;; esac; }  # exited: zombie or being reaped
ok() { echo "ok   - $1"; pass=$((pass+1)); }
ng() { echo "FAIL - $1${2:+: $2}"; fail=$((fail+1)); }
trap '"$BIN" -socket "$SOCK" -stop >/dev/null 2>&1; rm -rf "$T"' EXIT
dpid() { pgrep -f -- "-socket $SOCK" | head -1; }

[[ $("$BIN" -version) == "wsl2-ssh-agent "* ]] && ok "-version" || ng "-version"
"$BIN" -bogus >/dev/null 2>&1; [ $? = 2 ] && ok "unknown flag -> exit 2" || ng "unknown flag"
"$BIN" -socket >/dev/null 2>&1; [ $? = 2 ] && ok "missing flag value -> exit 2" || ng "missing value"
"$BIN" -socket "$SOCK" -helper /nonexistent >/dev/null 2>&1; [ $? = 1 ] && [ ! -e "$SOCK" ] && ok "bad helper -> exit 1, no socket" || ng "bad helper"
"$BIN" -socket "$T/$(head -c 120 /dev/zero | tr '\0' x)" -helper "$T/echo-helper" >/dev/null 2>&1; [ $? = 1 ] && ok "over-long socket path rejected" || ng "long path"

out=$(timeout 5 "$BIN" "${A[@]}" --pipename=my-pipe)
[ "$out" = "SSH_AUTH_SOCK=$SOCK; export SSH_AUTH_SOCK;" ] && ok "daemonizes and prints snippet (--flag=value syntax)" || ng "daemonize" "$out"
P=$(dpid); [ -n "$P" ] || { ng "daemon pid"; exit 1; }
[ "$(stat -c %a "$SOCK")" = 600 ] && ok "socket 0600" || ng "socket mode"
[ "$(ps -o sid= -p "$P" | tr -d ' ')" = "$P" ] && ok "daemon is a session leader (setsid)" || ng "setsid"
[ "$(readlink /proc/$P/fd/0)" = /dev/null ] && [ "$(readlink /proc/$P/fd/1)" = /dev/null ] && ok "stdio detached" || ng "stdio"

r=$(printf 'hello agent' | "$SC" "$SOCK"); [ "$r" = "hello agent" ] && ok "client socket handed to helper as stdin/stdout" || ng "echo" "$r"
sleep 0.2; grep -q 'helper args: my-pipe' "$T/log" && ok "pipe name passed to helper, helper stderr -> log" || ng "helper args"
big=$(head -c 200000 /dev/urandom | base64 -w0); r=$(printf '%s' "$big" | "$SC" "$SOCK"); [ "$r" = "$big" ] && ok "200 KB round trip" || ng "large"

for i in $(seq 32); do (r=$(printf "c$i" | "$SC" "$SOCK"); [ "$r" = "c$i" ] || echo bad) & done > "$T/conc" 2>&1; wait
[ ! -s "$T/conc" ] && ok "32 concurrent clients" || ng "concurrency" "$(cat "$T/conc")"

fd0=$(ls /proc/$P/fd | wc -l); rss0=$(awk '/^VmRSS/{print $2}' /proc/$P/status)
for i in $(seq 300); do printf x | "$SC" "$SOCK" >/dev/null; done
sleep 0.3
[ -z "$(cat /proc/$P/task/*/children)" ] && ok "all helpers reaped after 300 connections" || ng "children left"
[ "$(ls /proc/$P/fd | wc -l)" = "$fd0" ] && ok "no fd leak" || ng "fd leak"
rss1=$(awk '/^VmRSS/{print $2}' /proc/$P/status)
if [ $SANITIZED = 1 ]; then ok "RSS growth check skipped (sanitizer build: allocator bookkeeping grows; LSan checks leaks)"
else [ $((rss1-rss0)) -le 64 ] && ok "no RSS growth ($rss0 -> $rss1 kB)" || ng "rss" "$rss0 -> $rss1"; fi
[ "$(ls /proc/$P/task | wc -l)" = 1 ] && ok "single thread" || ng "threads"
c0=$(awk '{print $14+$15}' /proc/$P/stat); v0=$(awk '/^voluntary/{print $2}' /proc/$P/status); sleep 3
[ "$(awk '{print $14+$15}' /proc/$P/stat)" = "$c0" ] && [ "$(awk '/^voluntary/{print $2}' /proc/$P/status)" = "$v0" ] && ok "zero CPU and zero wakeups while idle (3 s)" || ng "idle wakeups"

o=$("$BIN" -socket "$SOCK" -format fish); [ "$o" = "set -x SSH_AUTH_SOCK $SOCK" ] && [ "$(dpid)" = "$P" ] && ok "second start reuses daemon" || ng "reuse"
"$BIN" -socket "$SOCK" -stop; gone "$P" && [ ! -e "$SOCK" ] && ok "-stop" || ng "-stop"

"$BIN" "${A[@]}" >/dev/null; P=$(dpid); kill -9 "$P"; sleep 0.1
"$BIN" "${A[@]}" >/dev/null; P2=$(dpid); r=$(printf ok | "$SC" "$SOCK")
[ "$P2" != "$P" ] && [ "$r" = ok ] && ok "stale socket replaced" || ng "stale"
kill -TERM "$P2"; sleep 0.2; [ ! -e "$SOCK" ] && ok "SIGTERM removes socket" || ng "SIGTERM"

"$BIN" "${A[@]}" -foreground & FP=$!; sleep 0.2
r=$(printf fg | "$SC" "$SOCK"); kill -INT $FP; wait $FP; rc=$?
[ "$r" = fg ] && [ $rc = 0 ] && [ ! -e "$SOCK" ] && ok "foreground mode + SIGINT" || ng "foreground" "$r rc=$rc"

# ---- -prespawn (warm standby) ----
"$BIN" "${A[@]}" -prespawn >/dev/null; P=$(dpid); sleep 0.2
kids() { cat /proc/$P/task/*/children | wc -w; }
[ "$(kids)" = 1 ] && ok "prespawn: one standby helper while idle" || ng "prespawn standby" "$(kids)"
r=$(printf 'warm' | "$SC" "$SOCK"); [ "$r" = warm ] && ok "prespawn: client served by standby (half-close relayed)" || ng "prespawn echo" "$r"
r=$(printf '%s' "$big" | "$SC" "$SOCK"); [ "$r" = "$big" ] && ok "prespawn: 200 KB round trip through relay" || ng "prespawn large"
for i in $(seq 32); do (r=$(printf "p$i" | "$SC" "$SOCK"); [ "$r" = "p$i" ] || echo bad) & done > "$T/conc" 2>&1; wait
[ ! -s "$T/conc" ] && ok "prespawn: 32 concurrent clients" || ng "prespawn concurrency" "$(cat "$T/conc")"
for i in $(seq 200); do printf x | "$SC" "$SOCK" >/dev/null; done; sleep 0.3
[ "$(kids)" = 1 ] && ok "prespawn: back to exactly one standby after 200 connections" || ng "prespawn leftovers" "$(kids)"
spare=$(cat /proc/$P/task/*/children); kill "$spare"; sleep 0.3
n=$(cat /proc/$P/task/*/children); [ -n "$n" ] && [ "$n" != "$spare" ] && ok "prespawn: killed standby is replaced" || ng "prespawn respawn" "$n"
r=$(printf ok | "$SC" "$SOCK"); [ "$r" = ok ] && ok "prespawn: served after standby replacement" || ng "prespawn after respawn"
c0=$(awk '{print $14+$15}' /proc/$P/stat); v0=$(awk '/^voluntary/{print $2}' /proc/$P/status); sleep 3
[ "$(awk '/^voluntary/{print $2}' /proc/$P/status)" = "$v0" ] && ok "prespawn: zero wakeups while idle (3 s)" || ng "prespawn idle wakeups"
"$BIN" -socket "$SOCK" -stop; sleep 0.2
[ -z "$(pgrep -f "$T/echo-helper")" ] && ok "prespawn: standby exits with the daemon" || ng "standby left behind"

# a helper that dies immediately must not make the daemon spin
printf '#!/bin/sh\nexit 1\n' > "$T/dead-helper"; chmod 700 "$T/dead-helper"
"$BIN" -socket "$SOCK" -helper "$T/dead-helper" -log "$T/log2" -prespawn >/dev/null; P=$(dpid); sleep 3
v=$(awk '/^voluntary/{print $2}' /proc/$P/status)
[ "$v" -lt 40 ] && ok "prespawn: dying helper backs off (ctxsw=$v in 3 s)" || ng "respawn spin" "ctxsw=$v"
printf x | "$SC" "$SOCK" 3000 >/dev/null; [ $? != 3 ] && kill -0 "$P" && ok "prespawn: no standby -> direct path, no hang" || ng "prespawn fallback"
"$BIN" -socket "$SOCK" -stop

echo; echo "passed: $pass  failed: $fail"
[ $fail = 0 ]
