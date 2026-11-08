#!/bin/bash
# Usage: linmem.sh PID  -- RSS/PSS/anon/threads/CPU ticks of PID and all descendants
tree() { echo "$1"; for c in $(cat /proc/$1/task/*/children 2>/dev/null); do tree "$c"; done; }
tot_rss=0; tot_pss=0
for p in $(tree "$1"); do
  [ -r /proc/$p/smaps_rollup ] || continue
  rss=$(awk '/^Rss:/{print $2}' /proc/$p/smaps_rollup)
  pss=$(awk '/^Pss:/{print $2}' /proc/$p/smaps_rollup)
  anon=$(awk '/^Pss_Anon:/{print $2}' /proc/$p/smaps_rollup)
  hwm=$(awk '/^VmHWM:/{print $2}' /proc/$p/status)
  thr=$(awk '/^Threads:/{print $2}' /proc/$p/status)
  cpu=$(awk '{print $14+$15}' /proc/$p/stat)
  vcs=$(awk '/^voluntary_ctxt_switches/{v=$2}/^nonvoluntary/{n=$2}END{print v+n}' /proc/$p/status)
  printf "linux pid=%s comm=%s rss_kb=%s pss_kb=%s pss_anon_kb=%s hwm_kb=%s threads=%s cpu_ticks=%s ctxsw=%s\n" \
    "$p" "$(cat /proc/$p/comm)" "$rss" "$pss" "$anon" "$hwm" "$thr" "$cpu" "$vcs"
  tot_rss=$((tot_rss+rss)); tot_pss=$((tot_pss+pss))
done
echo "linux total rss_kb=$tot_rss pss_kb=$tot_pss"
