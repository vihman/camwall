#!/bin/bash
# One measurement of the running wall: CPU, load, clock and temperature, memory,
# the busiest threads and whether every camera stream keeps up. Prints no tokens
# and needs no root. Run it on the Pi while camwall.service is running.
#
#   tools/measure.sh [--settle SECONDS] [--samples N]
#
#   --settle   seconds to wait before measuring (default 0; use 180 right after a restart)
#   --samples  number of 5-second CPU samples (default 3)

set -euo pipefail

SETTLE=0; SAMPLES=3
while [ $# -gt 0 ]; do
  case $1 in
    --settle) SETTLE=$2; shift 2 ;;
    --samples) SAMPLES=$2; shift 2 ;;
    -h|--help) sed -n '2,9p' "${BASH_SOURCE[0]}" | sed 's/^# \{0,1\}//'; exit 0 ;;
    *) echo "unknown option: $1 (see --help)" >&2; exit 2 ;;
  esac
done

say() { printf '%-14s %s\n' "$1" "$2"; }

say date "$(date '+%F %T')"
say service "$(systemctl is-active camwall 2>/dev/null || true), $(systemctl show camwall -p StatusText --value 2>/dev/null || true)"
if [ "$SETTLE" -gt 0 ]; then echo "settling ${SETTLE}s ..."; sleep "$SETTLE"; fi

# CPU idle from top's second iteration, so each sample covers 5 s
idle=()
for _ in $(seq "$SAMPLES"); do
  idle+=("$(top -b -n 2 -d 5 | grep '^%Cpu' | tail -1 | sed -E 's/.* ([0-9.]+) id.*/\1/')")
done
say "cpu idle %" "${idle[*]} (avg $(printf '%s\n' "${idle[@]}" | awk '{s+=$1} END {printf "%.1f", s/NR}'))"
say load "$(cut -d' ' -f1-3 /proc/loadavg)"

if command -v vcgencmd >/dev/null; then
  say clock "$(vcgencmd measure_clock arm | awk -F= '{printf "%d MHz", $2/1e6}')"
  say temp "$(vcgencmd measure_temp | cut -d= -f2)"
  say throttled "$(vcgencmd get_throttled | cut -d= -f2)"
fi

awk '/^CmaTotal/ {t=$2} /^CmaFree/ {f=$2} END {printf "%-14s %d MB total, %d MB free, %d MB used\n", "cma", t/1024, f/1024, (t-f)/1024}' /proc/meminfo

# resident memory of the wall's processes (plane wall, or the GStreamer fallback)
rss=$( (ps -o rss= -C camwall-planes,ffmpeg,gst-launch-1.0 2>/dev/null || true) | awk '{s+=$1} END {print s+0}')
say "wall rss" "$((rss / 1024)) MB ($( (ps -o comm= -C camwall-planes,ffmpeg,gst-launch-1.0 2>/dev/null || true) | sort | uniq -c | awk '{printf "%s%s x%s", sep, $2, $1; sep=", "}'))"

echo
echo "busiest threads:"
top -b -n 2 -d 3 -H -w 200 | awk '/^top -/ {n++} n == 2' | sed -n '7,15p'

# Every camera stream: receive rate and the backlog in its socket. A backlog that
# grows between runs means the wall does not keep up (see docs/operation.md).
echo
echo "camera streams (10 s):"
snap() { ss -tni '( dport = :7441 )' | awk '
  /^ESTAB/ {q=$2; p=$4; next}
  /bytes_received:/ {match($0, /bytes_received:[0-9]+/); print p, q, substr($0, RSTART+15, RLENGTH-15)}'; }
a=$(snap); sleep 10; b=$(snap)
if [ -z "$a" ]; then
  echo "  no RTSPS sessions (wall idle?)"
else
  join <(echo "$a" | sort) <(echo "$b" | sort) | awk '{rate=($5-$3)/10;
    printf "  %-22s %6.0f kB/s %5.2f Mbit/s  Recv-Q %8d B  ~%.2f s queued\n",
           $1, rate/1000, rate*8/1e6, $4, (rate > 0 ? $4/rate : 0)}'
fi
