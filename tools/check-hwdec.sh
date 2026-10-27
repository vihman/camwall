#!/bin/bash
# Check that the camera streams are decoded by the Pi 4's HEVC hardware and not
# in software. Run it on the Pi while the wall is running; it re-runs itself with
# sudo (needed to see the service's open files and to read the config for --ab).
#
#   tools/check-hwdec.sh [--ab]
#
#   --ab   also decode 10 s of camera 1 with and without the hardware decoder and
#          compare the CPU time (for 10 s each, ffmpeg's command line, which other
#          local users can see, contains camera 1's token)

set -euo pipefail

AB=0
for a in "$@"; do
  case $a in
    --ab) AB=1 ;;
    -h|--help) sed -n '2,10p' "${BASH_SOURCE[0]}" | sed 's/^# \{0,1\}//'; exit 0 ;;
    *) echo "unknown option: $a (see --help)" >&2; exit 2 ;;
  esac
done
[ "$EUID" -eq 0 ] || exec sudo "${BASH_SOURCE[0]}" "$@"

# 1. the decoder device
dev=
for n in /sys/class/video4linux/video*; do
  [ "$(cat "$n/name" 2>/dev/null)" = rpi-hevc-dec ] && { dev=/dev/${n##*/}; break; }
done
if [ -z "$dev" ]; then
  echo "no rpi-hevc-dec V4L2 device: H.265 can only be decoded in software"; exit 1
fi
echo "decoder:   $dev (rpi-hevc-dec)"

# 2. which processes hold it open (the plane wall: one process for all cameras;
#    the GStreamer wall: one ffmpeg per camera)
holders=$(fuser "$dev" 2>/dev/null | tr -s ' ' '\n' | grep -E '^[0-9]+$' || true)
if [ -z "$holders" ]; then
  echo "held by:   nobody (wall idle, or decoding in software)"
else
  echo "held by:   $(for p in $holders; do printf '%s(%s) ' "$(ps -o comm= -p "$p")" "$p"; done)"
fi

# 3. decoder interrupts over 5 s. About 2 interrupts per frame (the decoder works
#    in two phases; inferred from the measurements, not documented).
irqs() { awk 'tolower($0) ~ /feb00000|hevc|rpivid/ {for (i = 2; i <= NF && $i ~ /^[0-9]+$/; i++) s += $i} END {print s+0}' /proc/interrupts; }
i1=$(irqs); sleep 5; i2=$(irqs)
rate=$(( (i2 - i1) / 5 ))
echo "interrupts: ${rate}/s, about $((rate / 2)) decoded frames/s in total"

# 4. this ffmpeg can use the hardware decoder at all
if ffmpeg -hide_banner -hwaccels 2>/dev/null | grep -qx drm; then
  echo "ffmpeg:    drm hwaccel available"
else
  echo "ffmpeg:    no drm hwaccel (the GStreamer fallback would decode in software)"
fi

# 5. optional A/B: CPU seconds for 10 s of camera 1 with and without the decoder
if [ "$AB" = 1 ]; then
  set -a; . /etc/camwall/camwall.conf; set +a
  for opt in "-hwaccel drm" ""; do
    # shellcheck disable=SC2086  # $opt is two words or none
    t=$(ffmpeg -nostdin -hide_banner -loglevel info -benchmark -rtsp_transport tcp $opt \
          -i "rtsps://$UNIFI_HOST:7441/$TOKEN1" -an -t 10 -f null - 2>&1 |
        tr '\r' '\n' | grep -oE 'utime=[0-9.]+s stime=[0-9.]+s' | tail -1 || true)
    echo "a/b:       ${opt:-software}: $t"
  done
fi
