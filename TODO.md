# TODO

Open work only. Performance items need before/after numbers on the Pi (several samples, not single readings) before they are adopted. Background for each item is in [docs/design.md](docs/design.md).

## Phase 1: Plane wall against the GStreamer wall (done)

Passed on 2026-10-26: about a twelfth of the CPU at the full camera rate, a dead camera blanks only its tile and recovers, survives a monitor power cut, runs without root, stable for 7 h overnight. Details in [docs/measurements.md](docs/measurements.md) and [docs/design.md](docs/design.md#the-plane-wall). The plane wall is now the default, run by camwalld.

## Phase 2: Roll out camwalld and test it on the Pi (done)

Passed on 2026-10-26/27: camwalld runs the plane wall from boot with the monitor on or off, stops it on a monitor power cut and restarts it when power returns, is restarted by the systemd watchdog when frozen, and switches to the GStreamer fallback with `WALL=gstreamer`. One wall process ran 22.5 h through a dusk and a dawn with no restart, no leak and no false `display lost`. `install.sh` rebuilds reproducibly, and `tools/` holds the measuring scripts (`measure.sh` and `feed-info.sh` used on the Pi). Details in [docs/measurements.md](docs/measurements.md#daemon-camwalld) and [docs/operation.md](docs/operation.md).



## Phase 3: Home Assistant (after the wall is stable)

Design in [docs/design.md](docs/design.md#planned-home-assistant-integration).
- [ ] Per-camera status lines (`cam1 up`/`cam1 down`) that the daemon can report as health (feeds the MQTT entity in Phase 3).
- [ ] MQTT in the daemon (`python3-paho-mqtt` from apt):
  - discovery `select` entity (`cameras`, `info`, `off`), with availability and health
  - its own Mosquitto user, credentials in `/etc/camwall/` (0600)
- [ ] Remote button in HA, mapped to the select entity. Candidate: a Sonoff Zigbee button through Zigbee2MQTT instead of the BILRESA; details to be agreed.
- [ ] Info mode: run a Chromium kiosk under `cage` as a daemon child, mutually exclusive with the camera wall. Authenticate with a non-admin HA user's long-lived token, or with `trusted_networks` limited to the Pi.
- [ ] Later modes: a single camera full screen.

## Phase 4: Repository hygiene (any time)

- [ ] Lint, as pre-commit and a GitHub Actions workflow:
  - `shellcheck bin/camwall install.sh` and `pyflakes bin/camwalld`
  - `systemd-analyze verify` on the units
  - a compile of the C helper against Debian libdrm/libav headers
  - a markdown lint

## Before production

- [ ] Replace the SD card (an unbranded 8 GB card) with a branded high-endurance one. After an unclean shutdown, 8 library files from 6 packages failed `dpkg --verify`. One of them, `libXrandr`, stopped `ddcutil` from starting. The packages were reinstalled on 2026-10-26. Until the card is replaced, re-check with `sudo dpkg --verify 2>/dev/null | grep -v ' c /'` now and then; only `g_ether.conf` is expected.
- [ ] H.264 cameras via `h264_v4l2m2m` (added 2026-10-27): all four cameras on H.264 run with `gpu_mem=256` (the default 76 MB is not enough), at the same CPU as H.265. Still to check: the tile's recovery after a camera restart; whether a smaller `gpu_mem` (128) suffices; and whether `avcodec_send_packet` returning `EAGAIN` (possible with v4l2m2m when its input queue is full) drops packets, since the loop does not resend them.
- [ ] Restrict devices: check the `char-*` class names in `/proc/devices` (drm, video4linux, media, dma_heap, i2c), add `DevicePolicy=closed` + `DeviceAllow=` to the unit, and test both walls.
- [ ] Consider `SystemCallFilter=@system-service` and `MemoryDenyWriteExecute=`. Python and the plane wall should cope; the GStreamer fallback may not (ORC JIT), so test it too.
- [ ] Not yet tried with the new URLs: the GStreamer wall, `check-hwdec.sh --ab`, a non-UniFi source.

## GStreamer wall (fallback only)

Only worth doing if the fallback is used regularly or the plane wall is abandoned.

- [ ] Verify on the Pi:
  - a stalled stream makes ffmpeg exit within `STREAM_TIMEOUT`
  - the preflight's `/proc/PID/fd` check sees `/dev/video19`

- [ ] GStreamer wall: raise the FIFO pipe size (`F_SETPIPE_SZ`) or use Unix sockets to cut the `filesrc` overhead, then re-run 30 fps. Without it, 30 fps saturates the CPU on Ethernet too (2026-10-26).
- [ ] GStreamer wall: add black fallback tiles (`videotestsrc` + compositor `ignore-inactive-pads`), so one dead camera no longer blanks the whole wall.
- [ ] Hardware H.264 for H.264 streams: `h264_v4l2m2m`, chosen per camera via `ffprobe`.
