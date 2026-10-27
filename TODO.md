# TODO

Open work only. Performance items need before/after numbers on the Pi (several samples, not single readings) before they are adopted. Background for each item is in [docs/design.md](docs/design.md).

## Phase 1: Plane wall against the GStreamer wall (done)

Passed on 2026-10-26: about a twelfth of the CPU at the full camera rate, a dead camera blanks only its tile and recovers, survives a monitor power cut, runs without root, stable for 7 h overnight. Details in [docs/measurements.md](docs/measurements.md) and [docs/design.md](docs/design.md#the-plane-wall). The plane wall is now the default, run by camwalld.

## Phase 2: Roll out camwalld and test it on the Pi

- [x] Rolled out 2026-10-26: camwalld runs the plane wall, and a second `./install.sh` reports `camwall-planes` as up to date, so the build is reproducible.
- [x] Boot tests passed 2026-10-26:
  - **Monitor on:** the wall starts about 1 s after the service, with no login, as user `camwall`.
  - **Monitor off:** `no display detected (Display not found)`, and no wall runs. After plugging the monitor in, the daemon starts the wall, which sets 1920x1080 itself, and all four cameras stream. The 1024x768 cropping of the GStreamer wall cannot happen, because the plane wall sets the monitor's preferred mode rather than using the console's.
- [x] Under the daemon, passed 2026-10-26:
  - **Plug cut while running:** `display lost` after about 15 s, the wall stops, and it returns when power does. `PROBE_SETTLE` is now counted from the end of the slow ddcutil probe.
  - **Watchdog:** a frozen daemon (`systemctl kill -s STOP --kill-whom=main`) is restarted by systemd about 34 s later (30 s watchdog plus 3 s restart delay).
  - **Fallback:** `WALL=gstreamer` runs the GStreamer wall, and removing it switches back.
- [x] Soak under camwalld, 2026-10-26 10:57 to 2026-10-27 09:24: one wall process for 22.5 h with no restart, through a dusk and a dawn. 74.5 MB resident and 440 MB CMA free at the end. That is the same level as the previous run after its first dawn, so the dawn step is a one-time buffer growth, not a leak. No false `display lost` with the monitor on. A dead camera under camwalld blanks only its own tile (several cameras restarted on 2026-10-26).
- [ ] Restrict devices: check the `char-*` class names in `/proc/devices` (drm, video4linux, media, dma_heap, i2c), add `DevicePolicy=closed` + `DeviceAllow=` to the unit, and test both walls.
- [ ] Consider `SystemCallFilter=@system-service` and `MemoryDenyWriteExecute=`. Python and the plane wall should cope; the GStreamer fallback may not (ORC JIT), so test it too.
- [x] `tools/measure.sh`, `tools/check-hwdec.sh` and `tools/feed-info.sh` added (2026-10-27). The feed parsing was tested against stub ffprobe output; still to run on the Pi.

## Phase 2b: C helper improvements

- [x] Split `planes/` into `common.h`, `stream.c`, `display.c`, `main.c` and a `Makefile`, with separate `Stream`, `Tile` and `PlaneProps` structs (2026-10-26). No warnings with `-Wall -Wextra`. On the Pi, all four tiles stream, and restarting several cameras blanks and recovers only their own tiles.
- [ ] Per-camera status lines (`cam1 up`/`cam1 down`) that the daemon can report as health (feeds the MQTT entity in Phase 3).
- [ ] Share one hardware device context across the camera threads.
- [ ] H.264 cameras via `h264_v4l2m2m` (added 2026-10-27, untested): on the Pi check the `streaming (h264_v4l2m2m)` line, a correct picture, the tile's recovery after a camera restart, and the CPU with one and with four cameras on H.264.

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
  - `shellcheck bin/camwall install.sh` and `pyflakes bin/camwalld`
  - `systemd-analyze verify` on the units
  - a compile of the C helper against Debian libdrm/libav headers
  - a markdown lint
- [ ] Bash version guard in `bin/camwall`: `wait -n -p` needs bash 5.1+, so fail early with a clear message.
- [ ] Add a LICENSE.

## Before production

- [ ] Replace the SD card (an unbranded 8 GB card) with a branded high-endurance one. After an unclean shutdown, 8 library files from 6 packages failed `dpkg --verify`. One of them, `libXrandr`, stopped `ddcutil` from starting. The packages were reinstalled on 2026-10-26. Until the card is replaced, re-check with `sudo dpkg --verify 2>/dev/null | grep -v ' c /'` now and then; only `g_ether.conf` is expected.
- [ ] Regenerate all four RTSP tokens in Protect and update `/etc/camwall/camwall.conf`.

## GStreamer wall (fallback only)

Only worth doing if the fallback is used regularly or the plane wall is abandoned.

- [ ] Verify on the Pi:
  - a stalled stream makes ffmpeg exit within `STREAM_TIMEOUT`
  - the preflight's `/proc/PID/fd` check sees `/dev/video19`

- [ ] GStreamer wall: raise the FIFO pipe size (`F_SETPIPE_SZ`) or use Unix sockets to cut the `filesrc` overhead, then re-run 30 fps. Without it, 30 fps saturates the CPU on Ethernet too (2026-10-26).
- [ ] GStreamer wall: add black fallback tiles (`videotestsrc` + compositor `ignore-inactive-pads`), so one dead camera no longer blanks the whole wall.
- [ ] Hardware H.264 for cameras with Super Encoding unchecked: `h264_v4l2m2m`, chosen per camera via `ffprobe`.
