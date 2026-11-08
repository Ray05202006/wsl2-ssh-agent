#!/bin/bash
# Run the daemon tests and the helper's framing tests under valgrind memcheck:
# uninitialised reads, invalid accesses, leaks, syscalls on invalid fds, on a normal
# (non-ASan) -O1 -g build.   tests/valgrind.sh [N fuzz seeds, default 300]
set -u
cd "$(dirname "$0")/.."
command -v valgrind >/dev/null || { echo "valgrind not installed"; exit 2; }
N=${1:-300}
T=$(mktemp -d "${XDG_RUNTIME_DIR:-/tmp}/wsa-vg.XXXXXX"); trap 'rm -rf "$T"' EXIT
VG=(valgrind -q --error-exitcode=99 --leak-check=full --errors-for-leak-kinds=definite,indirect --track-fds=no)
bad=0

# 1. daemon: every process (daemon, relays) logs to its own file
cc -O1 -g -Wall -DHELPER_EXE="\"$(realpath "${HELPER_EXE:-build/wsl2-ssh-agent-helper.exe}")\"" -o "$T/wsl2-ssh-agent" src/wsl2-ssh-agent.c || exit 1
printf '#!/bin/sh\nexec %s --log-file=%s/vg.%%p %s "$@"\n' "${VG[*]}" "$T" "$T/wsl2-ssh-agent" > "$T/bin"; chmod 700 "$T/bin"
VALGRIND=1 tests/daemon.sh "$T/bin" > "$T/daemon.out" 2>&1; rc=$?
grep -v '^ok' "$T/daemon.out"
[ $rc = 0 ] || bad=$((bad+1))
# --track-fds=yes would count every fd still open at exit (stdio, log,
# /dev/null; closed by the kernel) as an error. Fd leaks during operation are
# what daemon.sh's fd-count check after 300 connections measures; valgrind
# still warns about syscalls on invalid fds.
errs=$(cat "$T"/vg.* 2>/dev/null | grep -E 'Invalid|uninitialised|definitely lost|indirectly lost|invalid file descriptor|Syscall param' | sort | uniq -c)
if [ -n "$errs" ]; then echo "daemon: valgrind reports:"; echo "$errs"; bad=$((bad+1))
else echo "daemon: $(ls "$T"/vg.* | wc -l) processes checked, no memcheck errors"; fi

# 2. helper framing logic (Win32 shim), same generator/oracle as the fuzzer
cc -O1 -g -Itests/fuzz -Wall -Wno-unused-function -o "$T/helper" tests/fuzz/harness.c || exit 1
cc -O2 -o "$T/gen" tests/fuzz/gen.c || exit 1
for i in $(seq "$N"); do
	"$T/gen" "$i" "$T/in" "$T/exp" "$T/rep" $((i % 3 == 0))
	FAKE_AGENT=echo "${VG[@]}" "$T/helper" < "$T/in" > "$T/out" 2>"$T/err"; rc=$?
	case $rc in 0|4) ;; *) echo "seed $i echo: rc=$rc"; head -20 "$T/err"; bad=$((bad+1));; esac
	FAKE_AGENT=garbage FAKE_REPLY="$T/rep" "${VG[@]}" "$T/helper" < "$T/in" > /dev/null 2>"$T/err"; rc=$?
	case $rc in 0|3|4) ;; *) echo "seed $i garbage: rc=$rc"; head -20 "$T/err"; bad=$((bad+1));; esac
done
echo "helper: $((2 * N)) runs under memcheck"
echo "valgrind: $bad problem(s)"
[ $bad = 0 ]
