# Operation

How the running wall behaves and how to adjust it. The command summary is in the [README](../README.md#operation).

Under systemd the FIFOs live in `/run/camwall` (`RuntimeDirectory=`, removed when the service stops). Run by hand, the script falls back to a temporary directory. Do not run `camwall` by hand while the service is running. Two copies fight over the display and the camera connections. Stop the service first.

ffmpeg errors in the journal carry a `camN:` prefix, which tells you which camera failed.

## Changing the frame rate

`FPS` is read from the service's environment and defaults to 15. Set it in `/etc/camwall/camwall.conf` (the example file has a commented-out `#FPS=15` line) and restart the service:

```bash
sudoedit /etc/camwall/camwall.conf      # FPS=20
sudo systemctl restart camwall
```

It must be a whole number from 1 to 100; anything else stops the service with a configuration error (status 78). If an older install still has an `FPS` drop-in in `/etc/systemd/system/camwall.service.d/`, remove it, because two places setting the same variable is confusing.

The default of 15 fps is the conservative choice. On the reference Pi over Ethernet, 24 fps runs at about 28% idle CPU and keeps up, while 30 fps overloads the CPU and the picture lags (see [Measurements](measurements.md)). Over Wi-Fi, or on a Pi with weaker cooling, stay at 20 fps or lower. After changing it, check load, temperature and [whether the wall keeps up](#checking-that-the-wall-keeps-up).

Check that it took effect. The command prints only the `fps=` part of each ffmpeg command line, so no token is shown; expect one line, `4 fps=20`. It checks every ffmpeg process on the machine, so the count assumes no other ffmpeg processes are running:

```bash
ps -o args= -p "$(pgrep -d, -x ffmpeg)" | grep -oE 'fps=[0-9]+' | sort | uniq -c
```

## Checking that the wall keeps up

After ffmpeg the pipeline cannot build up a delay: each FIFO holds less than one frame, the GStreamer queues are leaky and hold at most 4 frames, and `kmssink` does not wait for a clock. If the Pi is too slow, the backlog builds up in front of ffmpeg, in the camera sockets' receive queues. This estimates the queued delay per camera from the queued bytes and the stream's receive rate (no `sudo` needed, no tokens shown):

```bash
snap() { ss -tni '( dport = :7441 )' | awk '
  /^ESTAB/ {q=$2; p=$4; next}
  /bytes_received:/ {match($0,/bytes_received:[0-9]+/); print p, q, substr($0,RSTART+15,RLENGTH-15)}'; }
a=$(snap); sleep 10; b=$(snap)
join <(echo "$a" | sort) <(echo "$b" | sort) | awk '{rate=($5-$3)/10;
  printf "%-22s %6.0f kB/s  Recv-Q %8d B  ~%.2f s queued\n", $1, rate/1000, $4, (rate>0 ? $4/rate : 0)}'
```

About 0 s on every camera means the wall keeps up. Seconds that grow between runs mean it is falling behind. So does a `Recv-Q` stuck at a large value while that camera's rate drops: the socket buffer is full and the backlog has moved into the gateway, where it cannot be seen. The constant base delay (camera encoding, gateway, ffmpeg's startup buffering, typically a second or two) is not included. To measure it, photograph the monitor next to a clock showing seconds and compare with the camera's timestamp overlay.

## How failures are handled

The script runs the wall once and exits as soon as any ffmpeg process or the GStreamer pipeline dies. systemd restarts it after 3 seconds, with no limit on retries. Repeated failures back off: the delay grows in four steps from 3 s to a maximum of 60 s (`RestartSteps=`, `RestartMaxDelaySec=`), so a broken gateway or network is not hammered. A camera that goes silent without closing its connection is caught by ffmpeg's socket timeout (`STREAM_TIMEOUT`, default 10 s, passed as `-timeout`): ffmpeg exits and the wall restarts like for any other dead camera. A configuration error (missing `UNIFI_HOST` or token, invalid `FPS` or `PROBE_*` value) exits with status 78 and is not restarted at all; the journal says what to fix, and after fixing it run `sudo systemctl restart camwall`. The compositor waits for all four inputs, so one camera that stays offline keeps the screen blank until it returns. Showing black tiles for dead cameras would need a different design; the decode-to-plane wall (see [Design](design.md#decode-to-plane-experimental)) gives it for free.

## Startup checks

At start the script checks that `ffmpeg` and `gst-launch-1.0` exist (fatal if not) and warns in the journal if no `rpi-hevc-dec` V4L2 device is found, if the service user cannot open it, or if this ffmpeg has no `drm` hwaccel; in those cases H.265 is decoded in software. About 10 s after the wall starts it also checks that each ffmpeg holds the decoder device open and logs `camN: ffmpeg does not hold ...` for one that does not, which is expected for a camera on H.264 (Standard). These are warnings only. They automate the `fuser` check in [Confirming the hardware decoder is in use](installation.md#confirming-the-hardware-decoder-is-in-use).

## Display presence

The wall also depends on the monitor being powered. While it runs, the script probes the display every `PROBE_INTERVAL` seconds (default 5) with `DISPLAY_PROBE` (default `ddcutil --syslog never getvcp d6`; `--syslog never` keeps ddcutil's own messages out of the journal). After `PROBE_FAILS` consecutive failures (default 3, so about 15 s) it stops all ffmpeg processes and the pipeline, which also ends the RTSP sessions, and idles, probing until the monitor answers again. It then waits `PROBE_SETTLE` seconds and starts the wall. The service stays `active` while idle; the journal shows `no display detected, idle until it returns` and `display detected, starting the wall`. If the Pi boots with the monitor off, the wall simply starts later. This replaces nothing in the failure handling above: a dead camera still exits the script and systemd restarts it. If the probe command is not installed, the script logs that and runs without the check; `DISPLAY_PROBE=none` turns it off explicitly. All four settings are optional and go in `/etc/camwall/camwall.conf`.

Why DDC/CI and not the HDMI connector status, and what the probe costs: see [Design](design.md#display-presence-detection).
