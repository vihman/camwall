# Operation

How the running wall behaves and how to adjust it. The command summary is in the [README](../README.md#operation).

`camwall.service` runs `camwalld`, a small daemon that starts the wall as a child process and supervises it. The wall is `camwall-planes` (`WALL=planes`, the default) or the GStreamer wall `camwall` (`WALL=gstreamer`). Everything the daemon and the wall print goes to the journal (`journalctl -u camwall`). `systemctl status camwall` shows the daemon's current state on its `Status:` line, for example `planes wall running` or `no display, idle`.

Do not run a wall by hand while the service is running. Two copies fight over the display and the camera connections. Stop the service first.

## Display presence

The wall depends on the monitor being powered. The daemon probes the display every `PROBE_INTERVAL` seconds (default 5) with `DISPLAY_PROBE` (default `ddcutil --syslog never getvcp d6`; `--syslog never` keeps ddcutil's own messages out of the journal).

- **Monitor off at start:** if the first probe fails, the daemon logs `no display detected, idle until it returns` and starts nothing: no decoding, no RTSP sessions. A Pi that boots with the monitor off simply starts the wall later.
- **Monitor goes off while the wall runs:** after `PROBE_FAILS` consecutive failures (default 3, so about 15 s), the daemon logs `display lost` and stops the wall.
- **Monitor comes back:** the daemon logs `display detected, starting the wall in Ns`, waits `PROBE_SETTLE` seconds (default 3) and starts the wall.

The service stays `active` throughout, and the journal and the `Status:` line of `systemctl status camwall` include the probe's own message, for example `no display detected (Display not found)`. If the probe command is not installed, or cannot run at all (exit status 126 or 127, for example after a broken system update leaves a shared library missing), the daemon logs `display probe cannot run (...)` and runs the wall without the check: a wall that is always on is a better failure than a wall that never starts. `DISPLAY_PROBE=none` turns the check off explicitly. All four settings are optional and go in `/etc/camwall/camwall.conf`.

Why DDC/CI and not the HDMI connector status, and what the probe costs: see [Design](design.md#display-presence-detection).

## How failures are handled

- **One camera fails** (camera restart, network problem): only its tile goes black, about 3 s after its last frame. The plane wall reconnects that camera by itself, retrying about every 12 s, and the tile returns once the stream does. The other tiles are not affected.
- **The wall process exits:** the daemon restarts it after 2 s. Repeated failures back off to 4, 8, 16 and at most 30 s; the backoff resets once the wall has run for a minute.
- **The daemon hangs:** it tells systemd it is alive every second. If that stops for 30 s (`WatchdogSec=30`), systemd kills the service and restarts it.
- **The daemon exits:** systemd restarts the service after 3 s, backing off to at most 60 s.
- **A configuration error** (missing `CAMn_URL`, a `<NAME>` in a URL whose variable is not set, a URL that is not `rtsp://` or `rtsps://`, invalid `WALL` or `PROBE_*` value, or a wall program that is not installed): the daemon or the wall exits with status 78, and systemd does not restart it. The journal says what to fix; after fixing it, run `sudo systemctl restart camwall`.

## MQTT

Optional, off by default. With `MQTT=yes`, camwalld connects to the broker in `MQTT_HOST` and can be switched on and off over MQTT. Any MQTT 3.1.1 broker and any controller works (Home Assistant, Node-RED, openHAB, or `mosquitto_pub` by hand). The payloads are plain text. It needs `python3-paho-mqtt`, which `install.sh` installs.

| Setting | Default | Meaning |
|---|---|---|
| `MQTT` | `no` | `yes` turns MQTT on; with `no` the other settings are ignored |
| `MQTT_HOST` | (required with `MQTT=yes`) | Broker name or IP. With TLS, use the name the certificate is issued for, and make sure the Pi resolves it to the broker |
| `MQTT_PORT` | 1883, or 8883 with TLS | Broker port |
| `MQTT_USER`, `MQTT_PASSWORD` | (none) | Broker credentials |
| `MQTT_TLS` | `no` | `yes`: TLS, verifying the broker certificate |
| `MQTT_CA_FILE` | (system CAs) | CA certificate for a broker with its own CA; the `camwall` user must be able to read it |
| `MQTT_TOPIC` | `camwall` | Topic prefix; give each wall its own when there is more than one |
| `MQTT_CLIENT_ID` | `camwall-<hostname>` | MQTT client id |
| `MQTT_DISCOVERY` | `homeassistant` | Home Assistant discovery prefix; `none` turns discovery off |

Topics, with the default prefix:

| Topic | Direction | Payload |
|---|---|---|
| `camwall/mode/set` | to camwall | `cameras` (the 2x2 grid), `camera1`..`camera4` (that camera full screen) or `off`; anything else is logged and ignored |
| `camwall/mode` | from camwall, retained | the current mode |
| `camwall/status` | from camwall, retained | the same text as the `Status:` line of `systemctl status camwall`, e.g. `planes wall running`, `no display, idle (Display not found)`, `off` |
| `camwall/availability` | from camwall, retained | `online`, or `offline` (also sent by the broker as the Last Will if camwalld disappears) |

- **`off`** stops the wall, as if the monitor had lost power. The status becomes `off`, and no decoding or RTSP session runs. The monitor then shows the Pi's text console (the login prompt); switch the monitor itself off if that matters. The display probe keeps running.
- **`cameras`** shows the 2x2 grid; **`camera1`**..**`camera4`** show that one camera on the whole screen (its 1280x720 stream scaled up by the display controller, so a little softer than a tile). From `off`, either starts the wall again once a display is present.
- **Switching between `cameras` and `cameraN`** does not restart the wall: all four cameras keep decoding, and the switch takes effect at the next display tick. Only the plane wall can do this; with `WALL=gstreamer` the single-camera modes show the grid, and camwalld logs that once.
- **The mode persists:** camwalld saves it in `/var/lib/camwall/mode`, so `off` survives a restart, a reboot or a broker outage. With `MQTT=no` the saved mode is ignored and the wall always runs, since nothing could switch it back on.
- **A broker that is down never stops the wall:** camwalld logs the lost connection once and reconnects by itself (after 1 s, backing off to at most 60 s).

`tools/mqtt-test.py` checks the settings step by step and stops at the first failure. The steps are the settings, the name lookup, TCP, TLS (with the certificate's subject, issuer and expiry), the MQTT login, and the retained messages on camwall's topics. `--set off` or `--set cameras` also sends a command. It uses sudo to read the configuration and prints no password. When camwalld cannot connect, its log line names the cause too, for example `cannot connect to host:8883 (TLS certificate: IP address mismatch, ...)`.

From any machine with `mosquitto-clients`:

```bash
mosquitto_sub -h <broker> -u <user> -P <password> -v -t 'camwall/#'
mosquitto_pub -h <broker> -u <user> -P <password> -t camwall/mode/set -m off
```

### Home Assistant

With the default `MQTT_DISCOVERY=homeassistant`, a device named after `MQTT_TOPIC` appears in Home Assistant's MQTT integration as soon as camwalld connects. It has a **Mode** select (`cameras`, `camera1`..`camera4`, `off`) and a **Status** sensor, and both show as unavailable while camwalld is offline. With the Mosquitto broker add-on, create a user for the Pi in the add-on's configuration (or as a Home Assistant user) and put it in `MQTT_USER` and `MQTT_PASSWORD`. Automations, for example a button press, call `select.select_option` on the Mode entity.

## Switching to the GStreamer wall

Set `WALL=gstreamer` in `/etc/camwall/camwall.conf` and restart the service:

```bash
sudoedit /etc/camwall/camwall.conf      # WALL=gstreamer
sudo systemctl restart camwall
```

Remove the line, or set `WALL=planes`, to switch back. Use the fallback if the plane wall misbehaves after a system update. The plane wall takes H.265 and H.264; for any other codec it logs `no decoder for ...` and leaves that tile black.

## Measuring and checking

Three scripts in `tools/` print no URLs or tokens. Run them on the Pi from the repository checkout (they use `bin/camwalld --url` to read the camera URLs):

- `tools/measure.sh [--settle SECONDS] [--samples N]` prints:
  - CPU idle samples, load, clock, temperature and throttling
  - CMA memory and the wall's resident memory
  - the busiest threads
  - for each camera stream, its receive rate and socket backlog (the check below)

  Use `--settle 180` right after a restart. The stream check uses sudo, because only root sees which process owns which socket.
- `tools/check-hwdec.sh [--ab]` finds the HEVC decoder device and the processes holding it, and turns the decoder's interrupt rate into decoded frames per second. That figure is about 120 for four cameras at 30 fps; the factor of two interrupts per frame is inferred, not documented. It also checks that ffmpeg has the `drm` hwaccel. `--ab` additionally compares 10 s of camera 1 decoded with and without the hardware. It uses sudo.
- `tools/feed-info.sh N [SECONDS]` measures what camera N actually sends: codec, resolution, frame rate, bitrate and keyframe interval. With `-` instead of N it reads a URL from stdin, so another stream (for example a camera's higher-resolution one) can be checked before changing the configuration: `read -rs u; echo "$u" | tools/feed-info.sh -`. The URL may use the config's `<NAME>` variables, e.g. `rtsps://<STREAM_HOST>:<STREAM_PORT>/<token>`. It uses sudo to read the configuration. While it measures, ffprobe's command line contains the URL.

## Checking that the wall keeps up

`tools/measure.sh` runs this check as part of its output. Both walls read each camera's stream from a network socket. If a wall cannot keep up, the backlog builds up in the sockets' receive queues; nothing after the decoder can grow a delay (the plane wall always shows the newest frame, and in the GStreamer wall the FIFOs and leaky queues hold at most a few frames). This estimates the queued delay per camera from the queued bytes and the stream's receive rate. It picks the wall's connections by process (`camwall-planes`, or `ffmpeg` under the fallback), whatever their port, and shows no URLs:

```bash
snap() { sudo ss -tnpi state established | awk '
  /^[0-9]/ {p = ""}
  /^[0-9]/ && /"(camwall-planes|ffmpeg)"/ {q = $1; p = $3; next}
  p != "" && /bytes_received:/ {match($0, /bytes_received:[0-9]+/); print p, q, substr($0, RSTART+15, RLENGTH-15); p = ""}'; }
a=$(snap); sleep 10; b=$(snap)
join <(echo "$a" | sort) <(echo "$b" | sort) | awk '{rate=($5-$3)/10;
  printf "%-22s %6.0f kB/s  Recv-Q %8d B  ~%.2f s queued\n", $1, rate/1000, $4, (rate>0 ? $4/rate : 0)}'
```

About 0 s on every camera means the wall keeps up. Seconds that grow between runs mean it is falling behind. So does a `Recv-Q` stuck at a large value while that camera's rate drops: the socket buffer is full and the backlog has moved into the camera or recorder, where it cannot be seen. The constant base delay (camera encoding, recorder, the decoder's startup buffering, typically a second or two) is not included. To measure it, photograph the monitor next to a clock showing seconds and compare with the camera's timestamp overlay.

## The GStreamer wall (fallback)

Only relevant with `WALL=gstreamer`. This wall runs one ffmpeg per camera and a GStreamer pipeline, with FIFOs in `/run/camwall` (`RuntimeDirectory=`). ffmpeg errors in the journal carry a `camN:` prefix, which tells you which camera failed.

### Frame rate and tile size

`FPS` defaults to 15, a whole number from 1 to 100. On the reference Pi over Ethernet, 24 fps runs at about 28% idle CPU and keeps up, while 30 fps overloads the CPU and the picture lags (see [Measurements](measurements.md)). Over Wi-Fi, or on a Pi with weaker cooling, stay at 20 fps or lower. After changing it, check load, temperature and [whether the wall keeps up](#checking-that-the-wall-keeps-up). `TILE_WIDTH` and `TILE_HEIGHT` (default 960x540) must be half the display mode's width and height.

To check which frame rate is in effect, the command below prints only the `fps=` part of each ffmpeg command line, so no token is shown. Expect one line, for example `4 fps=20`. It assumes no other ffmpeg processes are running on the machine:

```bash
ps -o args= -p "$(pgrep -d, -x ffmpeg)" | grep -oE 'fps=[0-9]+' | sort | uniq -c
```

### Its own failure handling

The GStreamer wall exits as soon as any of its ffmpeg processes or the pipeline dies, and the daemon restarts it as described above. The compositor waits for all four inputs, so one camera that stays offline keeps the whole screen blank until it returns. A camera that goes silent without closing its connection is caught by ffmpeg's socket timeout (`STREAM_TIMEOUT`, default 10 s).

### Startup checks

At start the GStreamer wall checks that `ffmpeg` and `gst-launch-1.0` exist (fatal if not). It warns in the journal, and then decodes H.265 in software, if:

- no `rpi-hevc-dec` V4L2 device is found
- the service user cannot open it
- this ffmpeg has no `drm` hwaccel

About 10 s after starting, it also checks that each ffmpeg holds the decoder device open. It logs `camN: ffmpeg does not hold ...` for one that does not, which is expected for a camera on H.264 (Super Encoding unchecked). These checks only warn. They automate the `fuser` check in [Confirming the hardware decoder is in use](installation.md#confirming-the-hardware-decoder-is-in-use).
