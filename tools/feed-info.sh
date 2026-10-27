#!/bin/bash
# Show what a camera actually sends: codec, resolution, frame rate, bitrate and
# keyframe interval, measured over a few seconds of the live stream. Re-runs itself
# with sudo to read the gateway and tokens from /etc/camwall/camwall.conf.
#
#   tools/feed-info.sh N [SECONDS]     camera N (1-4) as configured
#   tools/feed-info.sh - [SECONDS]     a token read from stdin, e.g. another stream
#                                      of a camera: read -rs t; echo "$t" | tools/feed-info.sh -
#
# SECONDS defaults to 10. The token is never printed, but for the measuring time
# ffprobe's command line, which other local users can see, contains it.

set -euo pipefail

feed=${1:-}; secs=${2:-10}
case $feed in
  [1-4]|-) ;;
  -h|--help) sed -n '2,11p' "${BASH_SOURCE[0]}" | sed 's/^# \{0,1\}//'; exit 0 ;;
  *) echo "usage: tools/feed-info.sh N|- [SECONDS] (see --help)" >&2; exit 2 ;;
esac
[[ $secs =~ ^[0-9]+$ ]] && [ "$secs" -ge 2 ] || { echo "SECONDS must be a whole number >= 2" >&2; exit 2; }
[ "$EUID" -eq 0 ] || [ -n "${FEED_URL:-}" ] || exec sudo "${BASH_SOURCE[0]}" "$@"

if [ -n "${FEED_URL:-}" ]; then        # testing hook: any URL or file
  url=$FEED_URL; label=$FEED_URL
else
  set -a; . /etc/camwall/camwall.conf; set +a
  if [ "$feed" = - ]; then
    read -r token; label="token from stdin"
  else
    t=TOKEN$feed; token=${!t:-}; label="camera $feed"
  fi
  [ -n "$token" ] || { echo "no token for $label" >&2; exit 1; }
  url="rtsps://$UNIFI_HOST:7441/$token"
fi

echo "measuring $label for ${secs}s ..."
ffprobe -v error -rtsp_transport tcp -timeout 10000000 -select_streams v:0 \
    -read_intervals "%+$secs" \
    -show_entries stream=codec_name,profile,width,height,pix_fmt,avg_frame_rate:packet=pts_time,size,flags \
    -of compact=p=1 "$url" |
awk -F'|' '
  { delete f; for (i = 2; i <= NF; i++) { split($i, kv, "="); f[kv[1]] = kv[2] } }
  $1 == "stream" { for (k in f) s[k] = f[k] }
  $1 == "packet" && f["pts_time"] != "N/A" {
    t = f["pts_time"] + 0; n++; bytes += f["size"]
    if (n == 1 || t < t0) t0 = t
    if (n == 1 || t > t1) t1 = t
    if (f["flags"] ~ /^K/) { if (nk) gop += t - lastk; lastk = t; nk++ }
  }
  END {
    if (!n) { print "no video packets received"; exit 1 }
    d = t1 - t0
    printf "codec          %s (%s), %s\n", s["codec_name"], s["profile"], s["pix_fmt"]
    printf "resolution     %sx%s\n", s["width"], s["height"]
    printf "frame rate     %.1f fps measured (stream says %s)\n", (d > 0 ? (n - 1) / d : 0), s["avg_frame_rate"]
    printf "bitrate        %.2f Mbit/s average over %.1f s (%d packets)\n", (d > 0 ? bytes * 8 / d / 1e6 : 0), d, n
    if (nk > 1) printf "keyframes      every %.1f s\n", gop / (nk - 1)
    else        printf "keyframes      %d in %.1f s (measure longer for the interval)\n", nk, d
  }'
