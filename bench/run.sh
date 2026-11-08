#!/bin/bash
# Benchmark one wsl2-ssh-agent configuration with a fixed workload.
#
#   bench/run.sh NAME WIN_IMAGE 'COMMAND with {SOCK}' [IDLE_SECONDS]
#
#   WIN_IMAGE  Windows image name of the relay process (powershell.exe or the helper)
#   COMMAND    starts the agent in the foreground, listening on {SOCK}
#
# Requires: WSL interop, a running Windows ssh-agent with >=1 key, build/agentbench.
# Raw output is appended to bench/results/NAME.txt.
set -u
name=$1 img=$2 cmd=$3 idle=${4:-60}
here=$(cd "$(dirname "$0")" && pwd)
AB=${AB:-$here/../build/agentbench}
R=${XDG_RUNTIME_DIR:-/tmp}/wsa-bench; mkdir -p "$R" "$here/results"
SOCK=$R/$name.sock; rm -f "$SOCK"
out=$here/results/$name.txt; : > "$out"
winmem() { (cd /tmp && powershell.exe -NoProfile -ExecutionPolicy Bypass -File "$(wslpath -w "$here/winmem.ps1")" "$@" | tr -d '\r'); }
log() { echo "$*" | tee -a "$out"; }
now() { date +%s%N; }

log "# $name  $(date -Is)"
log "# cmd: $cmd"
pre=$(winmem "$img" | sed -n 's/.* pid=\([0-9]*\) .*/\1/p' | paste -sd,)   # pre-existing processes

t0=$(now)
eval "exec ${cmd//\{SOCK\}/$SOCK}" >/dev/null 2>&1 &
pid=$!
until SSH_AUTH_SOCK=$SOCK ssh-add -l >/dev/null 2>&1; do
	kill -0 $pid 2>/dev/null || { log "agent died"; exit 1; }
	sleep 0.01
done
log "startup_to_first_reply_ms=$(( ($(now) - t0) / 1000000 ))"

sleep 5
log "## idle (5 s after first request)"
"$here/linmem.sh" $pid | tee -a "$out"
winmem "$img" "$pre" | tee -a "$out"

log "## workloads"
bench() { "$AB" "$SOCK" "$1" "$2" "${3:-1}" "$R/$name.raw.$1" | tee -a "$out"; }
rm -f "$R/$name.raw."*
bench list 500          # one connection, ssh-add -l style requests
bench sign 300          # one connection, public-key auth signatures
bench conn 100          # new connection per request (= one ssh/ssh-add process each)
bench conn 20 8         # 8 concurrent clients, new connection each
bench sign 50 8         # 8 concurrent long-lived clients signing
# real OpenSSH tooling: ssh-add -l, and ssh-keygen -Y sign (list + sign through the agent)
SSH_AUTH_SOCK=$SOCK ssh-add -L | head -1 > "$R/$name.pub"
echo data > "$R/$name.data"
for tool in "ssh-add -l" "ssh-keygen -Y sign -q -n bench -f $R/$name.pub $R/$name.data"; do
	t=$(now); fails=0
	for i in $(seq 30); do rm -f "$R/$name.data.sig"; SSH_AUTH_SOCK=$SOCK $tool >/dev/null 2>&1 || fails=$((fails+1)); done
	log "tool=\"${tool%% -q*}\" n=30 fails=$fails avg_ms=$(( ($(now) - t) / 30000000 ))"
done

log "## after workloads"
"$here/linmem.sh" $pid | tee -a "$out"
winmem "$img" "$pre" | tee -a "$out"

# idle CPU: CPU ticks and context switches of the agent's whole Linux process tree
snap() { "$here/linmem.sh" $pid | awk '/^linux pid/{for(i=1;i<=NF;i++){split($i,a,"=");if(a[1]=="cpu_ticks")c+=a[2];if(a[1]=="ctxsw")s+=a[2]}}END{print c+0, s+0}'; }
w0=$(winmem "$img" "$pre" | sed -n 's/.*cpu_ms=\([0-9]*\).*/\1/p' | awk '{s+=$1}END{print s+0}')
read c0 s0 < <(snap); sleep "$idle"; read c1 s1 < <(snap)
w1=$(winmem "$img" "$pre" | sed -n 's/.*cpu_ms=\([0-9]*\).*/\1/p' | awk '{s+=$1}END{print s+0}')
log "## idle ${idle}s: linux_cpu_ticks=$((c1-c0)) linux_ctxsw=$((s1-s0)) win_cpu_ms=$(( ${w1:-0} - ${w0:-0} )) (the win figure includes WMI query noise only if the process is resident)"

kill $pid; wait $pid 2>/dev/null
log "# done"
