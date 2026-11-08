#!/bin/bash
# Functional and regression tests for wsl2-ssh-agent.
#
#   tests/run.sh [path/to/wsl2-ssh-agent]
#   PRESPAWN=1 tests/run.sh ...     same suite with -prespawn on every daemon
#
# Needs: WSL2 with interop, the Windows "OpenSSH Authentication Agent" service
# running with at least one key loaded, ssh-add/ssh-keygen in WSL, a C compiler.
# Tests that need things this script cannot do unattended (restarting the
# Windows service needs elevation; wsl --shutdown kills the test) are listed
# at the end as manual checks.
set -u
cd "$(dirname "$0")/.."
BIN=$(realpath "${1:-./wsl2-ssh-agent}")
grep -qa __asan_init "$BIN" && SANITIZED=1 || SANITIZED=0
T=$(mktemp -d "${XDG_RUNTIME_DIR:-/tmp}/wsa-test.XXXXXX")
IDLE_CHILDREN=0
if [ "${PRESPAWN:-0}" = 1 ]; then   # wrapper appends -prespawn; exec keeps pids/cmdlines intact
	printf '#!/bin/sh\nexec "%s" "$@" -prespawn\n' "$BIN" > "$T/agent"; chmod 700 "$T/agent"
	BIN=$T/agent IDLE_CHILDREN=1; echo "# mode: -prespawn"
fi
SOCK=$T/agent.sock
mkdir -p build
cc -O2 -o build/sockcat tests/sockcat.c && cc -O2 -pthread -o build/agentbench bench/agentbench.c || exit 1
SC=$PWD/build/sockcat AB=$PWD/build/agentbench
pass=0 fail=0
gone() { [ ! -e /proc/$1 ] || case "$(awk '{print $3}' /proc/$1/stat 2>/dev/null)" in Z|X|"") true;; *) false;; esac; }  # exited: zombie or being reaped
ok() { echo "ok   - $1"; pass=$((pass+1)); }
ng() { echo "FAIL - $1${2:+: $2}"; fail=$((fail+1)); }
check() { local d=$1; shift; if "$@" >/dev/null 2>&1; then ok "$d"; else ng "$d"; fi; }
hex() { od -An -tx1 -v | tr -d ' \n'; }
cleanup() { "$BIN" -socket "$SOCK" -stop >/dev/null 2>&1; rm -rf "$T"; }
trap cleanup EXIT
daemon_pid() { pgrep -f -- "-socket $SOCK" | head -1; }

# ---- CLI ----
[[ $("$BIN" -version) == "wsl2-ssh-agent "* ]] && ok "-version" || ng "-version"
check "-h exits 0" "$BIN" -h
"$BIN" -bogus >/dev/null 2>&1; [ $? -eq 2 ] && ok "unknown flag rejected" || ng "unknown flag rejected"
[ "$(SHELL=/usr/bin/fish "$BIN" -socket "$SOCK" -format fish -stop 2>/dev/null; echo $?)" = 1 ] && ok "-stop without daemon fails" || ng "-stop without daemon fails"

# ---- daemon start (the eval $(wsl2-ssh-agent) workflow) ----
start=$(date +%s%N)
out=$(timeout 5 "$BIN" -socket "$SOCK" -log "$T/log")
el=$(( ($(date +%s%N) - start) / 1000000 ))
[ "$out" = "SSH_AUTH_SOCK=$SOCK; export SSH_AUTH_SOCK;" ] && ok "daemon prints sh snippet and detaches (${el} ms)" || ng "daemon output" "$out"
P=$(daemon_pid)
[ -n "$P" ] && ok "daemon running (pid $P)" || { ng "daemon running"; exit 1; }
[ "$(stat -c %a "$SOCK")" = 600 ] && ok "socket mode 0600" || ng "socket mode" "$(stat -c %a "$SOCK")"
out2=$("$BIN" -socket "$SOCK" -format csh)
[ "$out2" = "setenv SSH_AUTH_SOCK $SOCK" ] && [ "$(daemon_pid)" = "$P" ] && ok "second start reuses running daemon (csh format)" || ng "second start" "$out2"
[ "$("$BIN" -socket "$SOCK" -format fish)" = "set -x SSH_AUTH_SOCK $SOCK" ] && ok "fish format" || ng "fish format"
[ "$(ls /proc/$P/task | wc -l)" = 1 ] && ok "daemon is single-threaded" || ng "threads"

# ---- protocol through real OpenSSH tools ----
export SSH_AUTH_SOCK=$SOCK
check "ssh-add -l lists keys" ssh-add -l
ssh-add -L | head -1 > "$T/key.pub"; echo "payload $$" > "$T/data"
if ssh-keygen -Y sign -q -n wsa-test -f "$T/key.pub" "$T/data" 2>/dev/null &&
   ssh-keygen -Y check-novalidate -n wsa-test -s "$T/data.sig" < "$T/data" >/dev/null 2>&1; then
	ok "ssh-keygen -Y sign via agent produces a valid signature"
else ng "ssh-keygen -Y sign via agent"; fi

# ---- raw protocol ----
r=$(printf '\0\0\0\1\x0b' | "$SC" "$SOCK" | head -c 5 | hex)
[ "${r:8:2}" = 0c ] && ok "REQUEST_IDENTITIES -> IDENTITIES_ANSWER" || ng "identities" "$r"
r=$(printf '\0\0\0\1\x0b\0\0\0\1\x0b' | "$SC" "$SOCK" | wc -c)
[ "$r" -gt 20 ] && [ $((r % 2)) = 0 ] && ok "two pipelined requests get two replies ($r B)" || ng "pipelining" "$r"
# Windows OpenSSH 9.x closes the connection on these instead of replying
# FAILURE (same through the original PowerShell relay); we only require a
# well-formed failure reply or a clean close, never a hang.
for m in '\0\0\0\1\xc8' '\0\0\0\x0a\x1b\0\0\0\x05bogus'; do
	r=$(printf "$m" | "$SC" "$SOCK" 3000 | hex); rc=$?
	case "$r" in ""|0000000105*|000000011c*) ok "msg $(printf "$m" | head -c5 | hex): failure reply or clean close (${r:-closed})";; *) ng "msg $m" "$r";; esac
done
r=$(printf '\x7f\xff\xff\xff\x0b' | "$SC" "$SOCK" 3000; echo " rc=$?")
[ "$r" = " rc=0" ] && ok "oversized length: connection closed, no reply" || ng "oversized" "$r"
r=$(printf '\0\0\0\0' | "$SC" "$SOCK" 3000; echo " rc=$?")
[ "$r" = " rc=0" ] && ok "zero length: connection closed" || ng "zero length" "$r"
r=$(printf '\0\0\0\x10\x0b' | "$SC" "$SOCK" 3000; echo " rc=$?")
[ "$r" = " rc=0" ] && ok "truncated message + EOF: closed, no hang" || ng "truncated" "$r"
head -c 300000 /dev/urandom | "$SC" "$SOCK" 3000 >/dev/null; [ $? != 3 ] && ok "300 KB of garbage: no hang" || ng "garbage"
check "daemon alive after malformed input" kill -0 "$P"

# ---- load, concurrency, leaks ----
fd0=$(ls /proc/$P/fd | wc -l); rss0=$(awk '/^VmRSS/{print $2}' /proc/$P/status)
check "500 requests on one connection" "$AB" "$SOCK" list 500
check "200 signatures on one connection" "$AB" "$SOCK" sign 200
check "16 concurrent clients x 10 connections" "$AB" "$SOCK" conn 10 16
check "8 concurrent signing clients x 50" "$AB" "$SOCK" sign 50 8
check "300 sequential connections" "$AB" "$SOCK" conn 300
nkids() { cat /proc/$P/task/*/children | wc -w; }
for i in $(seq 50); do [ "$(nkids)" = $IDLE_CHILDREN ] && break; sleep 0.1; done
[ "$(nkids)" = $IDLE_CHILDREN ] && ok "only $IDLE_CHILDREN helper process(es) left after clients disconnect" || ng "leftover children" "$(cat /proc/$P/task/*/children)"
fd1=$(ls /proc/$P/fd | wc -l); rss1=$(awk '/^VmRSS/{print $2}' /proc/$P/status)
[ "$fd0" = "$fd1" ] && ok "no fd leak ($fd0 -> $fd1)" || ng "fd leak" "$fd0 -> $fd1"
if [ $SANITIZED = 1 ]; then ok "RSS growth check skipped (sanitizer build; LSan checks leaks)"
else [ $((rss1 - rss0)) -le 64 ] && ok "no memory growth (RSS $rss0 -> $rss1 kB)" || ng "memory growth" "$rss0 -> $rss1"; fi
[ -z "$(ps -o stat= --ppid "$P" | grep Z)" ] && ok "no zombies" || ng "zombies"

# ---- stop / stale socket / restart ----
"$BIN" -socket "$SOCK" -stop >/dev/null 2>&1
gone "$P" && [ ! -e "$SOCK" ] && ok "-stop terminates daemon and removes socket" || ng "-stop"
"$BIN" -socket "$SOCK" >/dev/null; P=$(daemon_pid); kill -9 "$P"; sleep 0.2
[ -S "$SOCK" ] || ng "stale socket setup"
"$BIN" -socket "$SOCK" >/dev/null; P2=$(daemon_pid)
[ -n "$P2" ] && [ "$P2" != "$P" ] && ssh-add -l >/dev/null && ok "stale socket after SIGKILL is replaced" || ng "stale socket"
kill -TERM "$P2"; sleep 0.3
gone "$P2" && [ ! -e "$SOCK" ] && ok "SIGTERM: clean exit, socket removed" || ng "SIGTERM"

# ---- embedded helper is (re)installed ----
H=${XDG_RUNTIME_DIR:-$HOME/.cache}/wsl2-ssh-agent/wsl2-ssh-agent-helper.exe
[ -n "${XDG_RUNTIME_DIR:-}" ] || H=$HOME/.cache/wsl2-ssh-agent/wsl2-ssh-agent-helper.exe
if [ -f "$H" ]; then
	printf 'corrupt' > "$H"
	"$BIN" -socket "$SOCK" >/dev/null && ssh-add -l >/dev/null && [ "$(stat -c %s "$H")" -gt 1000 ] &&
		ok "corrupted helper is replaced on start" || ng "helper reinstall"
	"$BIN" -socket "$SOCK" -stop >/dev/null
	[ "$(stat -c %a "$H")" = 700 ] && ok "helper mode 0700" || ng "helper mode"
fi

# ---- Windows agent unavailable ----
"$BIN" -socket "$SOCK" -pipename wsa-test-no-such-pipe >/dev/null; P=$(daemon_pid)
msg=$(timeout 10 ssh-add -l 2>&1); rc=$?
[ $rc != 0 ] && [ $rc != 124 ] && [[ $msg == *"communication with agent failed"* ]] && ok "agent pipe missing: ssh-add fails cleanly (rc=$rc)" || ng "agent unavailable" "rc=$rc $msg"
check "daemon survives unavailable agent" kill -0 "$P"
"$BIN" -socket "$SOCK" -stop >/dev/null

echo
echo "passed: $pass  failed: $fail"
if [ $fail != 0 ] && [ -s "$T/log" ]; then
	echo "--- daemon/helper errors from $T/log:"
	grep -v 'ssh: connected\|standby helper started' "$T/log" | tail -20
fi
cat <<'EOF'

manual checks (not automatable here):
  * Windows agent restart: run `Restart-Service ssh-agent` in an elevated
    PowerShell while the daemon runs, then `ssh-add -l` in WSL must succeed
    without restarting wsl2-ssh-agent (every connection opens a fresh pipe).
  * WSL restart: `wsl --shutdown` from Windows, reopen the shell; the rc-file
    or systemd unit starts a new daemon and replaces the stale socket.
EOF
[ $fail = 0 ]
