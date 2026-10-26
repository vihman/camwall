# TODO

Open work only. Performance items need before/after numbers on the Pi (several samples, not single readings) before they are adopted. Background for each item is in [docs/design.md](docs/design.md).

## Phase 0: Housekeeping

- [ ] `drm-planes`: rebase onto `dev`. Its commit message says "never run on the Pi"; it has since run with all four cameras (recorded in docs/design.md).
- [ ] Verify on the Pi the parts of the current wall that were only checked offline or with stubs:
  - the hardened unit: wall comes up, `fuser -v /dev/video19` shows all four ffmpeg, display probe works, no permission errors in the journal
  - a stalled stream makes ffmpeg exit within `STREAM_TIMEOUT`. Disable one camera's RTSP in Protect: a firewall rule cannot single out one camera, because all four share gateway:7441
  - the restart delay grows on repeated failures, and whether it drops back to 3 s after a long healthy run
  - the preflight's `/proc/PID/fd` check sees `/dev/video19`
  - a several-hour display-probe soak with the monitor on finds no false failures; add retries or `--sleep-multiplier` if it does
- [ ] Reboot once and confirm that Wi-Fi stays off and the wall comes up over Ethernet (`sudo ss -tnp | grep :7441` shows the Ethernet address).

## Phase 1: Measure decode-to-plane against the GStreamer wall

Decision gate: Phase 2 starts only if the plane wall uses clearly less CPU, survives the dead-camera and soak tests and shows no memory growth.

- [ ] Add `tools/measure.sh`:
  - 3 `top -b -H` samples after a 60 s settle
  - `vcgencmd` clock, temperature and `get_throttled`
  - `CmaFree`/`CmaTotal` from `/proc/meminfo`
  
  Also add `tools/check-hwdec.sh` with the decoder checks from docs/installation.md: `fuser`, the interrupt counter, the A/B benchmark.
- [x] The plane wall runs as user `camwall` without root, survives a dead camera (only its tile goes black, it recovers by itself) and its device nodes are listed (docs/design.md).
- [ ] List the GStreamer wall's device nodes too (`sudo fuser -v /dev/dri/* /dev/video* /dev/media* /dev/dma_heap/* /dev/i2c-*`), and check the `char-*` class names in `/proc/devices`. Both feed `DevicePolicy=closed` + `DeviceAllow=` for the units.
- [ ] Robustness of the plane wall (dead camera and monitor plug cut already pass, see docs/design.md):
  - booting with the monitor off, once there is a unit that starts it at boot (Phase 2); the black-primary fallback is untested
  - memory trend from one continuous run: compare RSS and `CmaFree` after 7 h (73.2 MB, 442.6 MB free) with a reading 10+ hours later, same pid. Stability passed 7 h overnight.
- [ ] Latency of both walls: photograph the screen next to a reference clock.

## Phase 2: Productize the plane wall (C helper + Python daemon)

- [ ] C helper `planes/camwall-planes.c`. Keep it a thin child process: environment in, exit code out.
  - Print a status line on each camera state change (`cam1 up`/`cam1 down`).
  - Share one hardware device context across cameras.
  - Clean shutdown on SIGTERM.
  - Add a `Makefile`.
  - `install.sh` installs the `-dev` packages and `gcc`, then builds it on the Pi.
- [ ] Python daemon `bin/camwalld` (stdlib only, no pip):
  - config parsing and validation with exit 78, ported from `bin/camwall`
  - the DDC display-presence loop
  - starts and stops the helper and parses its status lines
  - `sd_notify` READY and WATCHDOG over `$NOTIFY_SOCKET`
- [ ] `systemd/camwalld.service`: `Type=notify`, `WatchdogSec=`, the same hardening as `camwall.service` plus the `DeviceAllow=` list from Phase 1, and `Conflicts=camwall.service` so either wall can be switched back instantly.
- [ ] Run both units side by side for a while. Then decide whether `bin/camwall` is retired or kept as the documented fallback, and update the README, docs and `install.sh`.
- [ ] Consider `SystemCallFilter=@system-service` and `MemoryDenyWriteExecute=` for the plane wall. They were skipped for the GStreamer wall because ORC JIT and ffmpeg may break under them.

## Phase 3: Home Assistant (after the wall is stable)

Design in [docs/design.md](docs/design.md#planned-home-assistant-integration).

- [ ] MQTT in the daemon (`python3-paho-mqtt` from apt):
  - discovery `select` entity (`cameras`, `info`, `off`), with availability and health
  - its own Mosquitto user, credentials in `/etc/camwall/` (0600)
- [ ] In HA:
  - enable the BILRESA event entities (they are disabled by default)
  - check their event types on the real device
  - map fixed button presses to fixed select targets, not the wheel
- [ ] Info mode: run a Chromium kiosk under `cage` as a daemon child, mutually exclusive with the camera wall. Authenticate with a non-admin HA user's long-lived token, or with `trusted_networks` limited to the Pi.
- [ ] Optional: fetch the RTSPS URLs through `uiprotect` (`python3-uiprotect`) instead of pasting tokens, which also makes rotation painless.
- [ ] Later modes: a single camera full screen.

## Phase 4: Repository hygiene (any time)

- [ ] Lint, as pre-commit and a GitHub Actions workflow:
  - `shellcheck bin/camwall install.sh`
  - `systemd-analyze verify` on the units
  - a compile of the C helper against Debian libdrm/libav headers
  - a markdown lint
- [ ] Bash version guard in `bin/camwall`: `wait -n -p` needs bash 5.1+, so fail early with a clear message.
- [ ] Add a LICENSE.

## Before production

- [ ] Regenerate all four RTSP tokens in Protect and update `/etc/camwall/camwall.conf`.

## Only if the plane wall is abandoned

- [ ] GStreamer wall: raise the FIFO pipe size (`F_SETPIPE_SZ`) or use Unix sockets to cut the `filesrc` overhead, then re-run 30 fps. Without it, 30 fps saturates the CPU on Ethernet too (2026-10-26).
- [ ] GStreamer wall: add black fallback tiles (`videotestsrc` + compositor `ignore-inactive-pads`), so one dead camera no longer blanks the whole wall.
- [ ] Hardware H.264 for cameras on Standard: `h264_v4l2m2m`, chosen per camera via `ffprobe`.
