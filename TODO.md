# TODO

Open work only. Performance items need before/after numbers on the Pi (several samples, not single readings) before they are adopted. Background for each item is in [docs/design.md](docs/design.md).

## Phase 1: Plane wall against the GStreamer wall (done)

Passed on 2026-10-26: about a twelfth of the CPU at the full camera rate, a dead camera blanks only its tile and recovers, survives a monitor power cut, runs without root, stable for 7 h overnight. Details in [docs/measurements.md](docs/measurements.md) and [docs/design.md](docs/design.md#the-plane-wall). The plane wall is now the default, run by camwalld.

## Phase 2: Roll out camwalld and test it on the Pi

- [x] Rolled out 2026-10-26: camwalld runs the plane wall, and a second `./install.sh` reports `camwall-planes` as up to date, so the build is reproducible.
- [x] Boot tests passed 2026-10-26:
  - **Monitor on:** the wall starts about 1 s after the service, with no login, as user `camwall`.
  - **Monitor off:** `no display detected (Display not found)`, and no wall runs. After plugging the monitor in, the daemon starts the wall, which sets 1920x1080 itself, and all four cameras stream. The 1024x768 cropping of the GStreamer wall cannot happen, because the plane wall sets the monitor's preferred mode rather than using the console's.
- [ ] Under the daemon:
  - a plug cut stops the wall after about 15 s, and restoring power brings it back
  - a dead camera still blanks only its own tile
  - no permission errors under the unit's hardening
  - the watchdog restarts a stopped daemon (`sudo systemctl kill -s STOP --kill-whom=main camwall`)
  - `WALL=gstreamer` switches to the fallback and back
  - no false `display lost` with the monitor on over a day (`journalctl -u camwall | grep -c 'display lost'`). If there are any, raise `PROBE_FAILS` or add `--sleep-multiplier` to the probe.
- [ ] Memory: confirm that the dawn step in the overnight run (+2.5 MB process memory, -6.5 MB CMA) does not repeat on every dusk and dawn. Log process memory and `CmaFree` every 5 min over at least one full day.
- [ ] Restrict devices: check the `char-*` class names in `/proc/devices` (drm, video4linux, media, dma_heap, i2c), add `DevicePolicy=closed` + `DeviceAllow=` to the unit, and test both walls.
- [ ] Consider `SystemCallFilter=@system-service` and `MemoryDenyWriteExecute=`. Python and the plane wall should cope; the GStreamer fallback may not (ORC JIT), so test it too.
- [ ] Add `tools/measure.sh`:
  - 3 CPU samples after a settle
  - clock, temperature and `get_throttled`
  - `CmaFree`/`CmaTotal`
  - the receive-queue check from docs/operation.md
  
  Also add `tools/check-hwdec.sh` with the decoder checks from docs/installation.md.

## Phase 2b: C helper improvements

- [ ] Split `planes/camwall-planes.c` for readability:
  - files: `stream.c` (RTSPS and decode), `display.c` (KMS planes and atomic commit), `main.c` (config and the frame-moving loop), plus a `Makefile`
  - split the mixed `Cam` struct into `Stream` and `Tile`
  - group the plane property IDs into one `PlaneProps` struct
  
  No behaviour change. Verify with the same CPU and dead-camera tests.
- [ ] Per-camera status lines (`cam1 up`/`cam1 down`) that the daemon can report as health (feeds the MQTT entity in Phase 3).
- [ ] Share one hardware device context across the camera threads.

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
- [ ] Hardware H.264 for cameras on Standard: `h264_v4l2m2m`, chosen per camera via `ffprobe`.
