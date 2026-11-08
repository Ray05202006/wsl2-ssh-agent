#!/bin/bash
# Randomized + boundary-case testing of the helper's framing logic, built for
# Linux against a Win32 shim, under ASan + UBSan.   tests/fuzz/run.sh [N]
set -u
cd "$(dirname "$0")"
N=${1:-3000}
T=$(mktemp -d "${XDG_RUNTIME_DIR:-/tmp}/wsa-fuzz.XXXXXX"); trap 'rm -rf "$T"' EXIT
SAN="-O1 -g -fsanitize=address,undefined -fno-sanitize-recover=all -fno-omit-frame-pointer"
cc $SAN -I. -Wall -Wno-unused-function -o "$T/helper" harness.c || exit 1
cc -O2 -o "$T/gen" gen.c || exit 1
export ASAN_OPTIONS=exitcode=99:detect_leaks=0 UBSAN_OPTIONS=halt_on_error=1:abort_on_error=0:exitcode=98
bad=0 runs=0
for i in $(seq "$N"); do
	old=$((i % 3 == 0))
	"$T/gen" "$i" "$T/in" "$T/exp" "$T/rep" $old
	# 1. echo agent: output must be exactly the expected frames
	#    (every 4th seed as a --ready standby: one 0xff byte first)
	if [ $((i % 4)) = 0 ]; then { printf '\377'; cat "$T/exp"; } > "$T/exp2"; mv "$T/exp2" "$T/exp"; export FAKE_READY=1; else unset FAKE_READY; fi
	if [ $old = 1 ]; then FAKE_OLD_AGENT=1 FAKE_AGENT=echo "$T/helper" < "$T/in" > "$T/out" 2>"$T/err"; rc=$?
	else FAKE_AGENT=echo "$T/helper" < "$T/in" > "$T/out" 2>"$T/err"; rc=$?; fi
	unset FAKE_READY
	runs=$((runs+1))
	case $rc in 0|4) ;; *) echo "seed $i echo: rc=$rc"; head -5 "$T/err"; bad=$((bad+1));; esac
	cmp -s "$T/out" "$T/exp" || { echo "seed $i echo: output mismatch ($(stat -c %s "$T/out") vs $(stat -c %s "$T/exp") B)"; bad=$((bad+1)); }
	# 2. hostile agent: anything goes, except memory errors, UB or a hang
	FAKE_AGENT=garbage FAKE_REPLY="$T/rep" timeout 10 "$T/helper" < "$T/in" > /dev/null 2>"$T/err"; rc=$?
	runs=$((runs+1))
	case $rc in 0|3|4) ;; *) echo "seed $i garbage: rc=$rc"; head -5 "$T/err"; bad=$((bad+1));; esac
done
# 3. pipe missing / permanently busy: must give up (virtual clock), never spin forever
for m in missing busy; do
	printf '\0\0\0\1\x0b' | FAKE_AGENT=$m timeout 10 "$T/helper" >/dev/null 2>"$T/err"; rc=$?; runs=$((runs+1))
	[ $rc = 2 ] || { echo "$m: rc=$rc"; bad=$((bad+1)); }
done
echo "fuzz: $runs runs, $bad problems"
[ $bad = 0 ]
