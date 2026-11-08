#!/bin/bash
# Real public-key SSH logins through an agent socket, against a throwaway
# user-level sshd on 127.0.0.1 that only accepts the agent's first key.
#   bench/sshauth.sh SOCK N [LABEL]   -> per-login wall time stats (ms)
set -u
SOCK=$1 N=$2 L=${3:-}
T=$(mktemp -d "${XDG_RUNTIME_DIR:-/tmp}/wsa-sshd.XXXXXX"); trap 'kill $(cat $T/pid 2>/dev/null) 2>/dev/null; rm -rf $T' EXIT
SSH_AUTH_SOCK=$SOCK ssh-add -L | head -1 > $T/authorized_keys
ssh-keygen -q -t ed25519 -N '' -f $T/hostkey
PORT=$((20000 + RANDOM % 20000))
cat > $T/cfg <<CFG
Port $PORT
ListenAddress 127.0.0.1
HostKey $T/hostkey
AuthorizedKeysFile $T/authorized_keys
PubkeyAuthentication yes
PasswordAuthentication no
KbdInteractiveAuthentication no
UsePAM no
StrictModes no
PidFile $T/pid
CFG
/usr/sbin/sshd -f $T/cfg -E $T/sshd.log || exit 1
sleep 0.3
o=(-F /dev/null -p $PORT -o IdentityAgent=$SOCK -o PreferredAuthentications=publickey -o BatchMode=yes
   -o UserKnownHostsFile=$T/kh -o StrictHostKeyChecking=accept-new -o ControlMaster=no)
ssh "${o[@]}" 127.0.0.1 true || { echo "ssh login failed"; tail $T/sshd.log; exit 1; }
for i in $(seq $N); do
	s=$(date +%s%N); ssh "${o[@]}" 127.0.0.1 true || echo FAIL >&2; echo $(( ($(date +%s%N) - s) / 1000 ))
done | sort -n | awk -v l="$L" '{a[NR]=$1} END {n=NR; printf "%s ssh_login n=%d min=%.1f p50=%.1f p95=%.1f p99=%.1f max=%.1f (ms)\n", l, n, a[1]/1e3, a[int((n-1)*.5)+1]/1e3, a[int((n-1)*.95)+1]/1e3, a[int((n-1)*.99)+1]/1e3, a[n]/1e3}'
