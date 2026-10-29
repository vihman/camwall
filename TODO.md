# TODO

Open work only. Performance items need before/after numbers on the Pi (several samples, not single readings) before they are adopted. Background for each item is in [docs/design.md](docs/design.md).

## Phase 1: Plane wall against the GStreamer wall (done)

Passed on 2026-10-26: about a twelfth of the CPU at the full camera rate, a dead camera blanks only its tile and recovers, survives a monitor power cut, runs without root, stable for 7 h overnight. Details in [docs/measurements.md](docs/measurements.md) and [docs/design.md](docs/design.md#the-plane-wall). The plane wall is now the default, run by camwalld.

## Phase 2: Roll out camwalld and test it on the Pi (done)

Passed on 2026-10-26/27: camwalld runs the plane wall from boot with the monitor on or off, stops it on a monitor power cut and restarts it when power returns, is restarted by the systemd watchdog when frozen, and switches to the GStreamer fallback with `WALL=gstreamer`. One wall process ran 22.5 h through a dusk and a dawn with no restart, no leak and no false `display lost`. `install.sh` rebuilds reproducibly, and `tools/` holds the measuring scripts (`measure.sh` and `feed-info.sh` used on the Pi). Details in [docs/measurements.md](docs/measurements.md#daemon-camwalld) and [docs/operation.md](docs/operation.md).



## Phase 3: Home Assistant (after the wall is stable)

Design in [docs/design.md](docs/design.md#home-assistant-integration).
- [ ] Per-camera status lines (`cam1 up`/`cam1 down`) that the daemon can report as health (feeds the MQTT entity in Phase 3).
- [ ] MQTT in the daemon (generic topics, optional TLS, HA discovery on by default; see [docs/operation.md](docs/operation.md#mqtt)). In use on the Pi over TLS with the HA Mosquitto add-on (2026-10-28). Still to check: the view surviving a restart and a monitor power cut, and a broker outage.
- [x] Remote button: a Sonoff Zigbee button through Zigbee2MQTT and an HA automation; single press steps through the views (`select.select_next`, cycling), long press returns to All cameras (2026-10-28). Double press is free for info mode.
- [ ] Info mode: an HA dashboard as an image (Puppet add-on on HA, `INFO_URL`), drawn on the frame buffer by camwalld (built 2026-10-29; the manual test on the Pi showed the image). Still to check on the Pi: switching from MQTT and the button automation in [operation.md](docs/operation.md#dashboard-info-mode), a monitor power cut while it shows, a restart with it saved, CPU while it shows. Then tune the dashboard (`dark`, `zoom`).
- [x] Single-camera modes `camera1`..`camera4`, switched live over the wall's stdin, and camera labels `CAM1_LABEL`..`CAM4_LABEL`: in use on the Pi from the HA select and a Zigbee button (2026-10-28).

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
- [ ] Not yet tried with the new URLs: `check-hwdec.sh --ab`, a non-UniFi source. (The GStreamer wall ran with them on 2026-10-28.)

## GStreamer wall (fallback only)

Only worth doing if the fallback is used regularly or the plane wall is abandoned.

- [ ] Verify on the Pi:
  - a stalled stream makes ffmpeg exit within `STREAM_TIMEOUT`
  - the preflight's `/proc/PID/fd` check sees `/dev/video19`

- [ ] GStreamer wall: raise the FIFO pipe size (`F_SETPIPE_SZ`) or use Unix sockets to cut the `filesrc` overhead, then re-run 30 fps. Without it, 30 fps saturates the CPU on Ethernet too (2026-10-26).
- [ ] With a newer GStreamer than 1.26.2 in Pi OS: retry hardware decoding in GStreamer itself (`rtph264depay ! h264parse ! v4l2h264dec` for H.264, `v4l2slh265dec` for the SAND picture), which would avoid the ffmpeg copy and software scaling. See [Things that did not work](docs/design.md#things-that-did-not-work).
- [ ] GStreamer wall: add black fallback tiles (`videotestsrc` + compositor `ignore-inactive-pads`), so one dead camera no longer blanks the whole wall.

## Ideas

- [ ] Sharper single cameras: an optional `CAMn_FULL_URL` (e.g. the camera's 4K High stream) that only the camera in single view uses, while the grid stays on Medium. One 4K camera plus three Medium ones decodes at full rate (2026-10-28, see [measurements](docs/measurements.md#a-4k-camera-in-the-plane-wall)), and only one camera is ever in single view, so all four could have a 4K single view. Switching would reconnect that camera (up to the 5 s keyframe interval), so keep showing the Medium picture until the first 4K frame arrives.

- [ ] More than four cameras (up to 16). Not measured; first test the Pi with the four URLs repeated to fill 9 and 16 tiles (`modetest -M vc4 -p` for the plane count; whether the kernel's HVS load check accepts 16 scaled planes; full frame rate).
  - Configuration: no camera count setting; the highest `CAMn_URL` set gives the count, a gap leaves a black tile, so cameras can be placed by number. Other `CAMn_` settings and `<NAME>` templates work for any `n`. `GRID=auto` (default) picks the smallest of 2x2, 3x3, 4x4 that holds the cameras; a four-camera config stays as now.
  - Layouts: square grids keep 16:9 tiles on a 16:9 screen with no bars (3x2 would show 640x360 pictures in 640x540 tiles). Later, named layouts with one large tile for camera 1, which also fill the screen: `1+5` (one 1280x720, five 640x360, on 3x3) and `1+7` (one 1440x810, seven 480x270, on 4x4). A layout is a table of blocks on the grid, so these are data, not new code paths.
  - Estimates: 16 Medium (720p30) streams are about 440 Mpx/s against the HEVC decoder's about 500, so use the Low stream (640x360, about the 4x4 tile size) for large grids; about 18 MB CMA and 3% of a core per camera. A large tile benefits from a sharper stream (see `CAMn_FULL_URL`).
  - Code: `MAXCAM` in `planes/common.h`, the fixed 2x2 in `planes/display.c`, `range(1, 5)` and `MODES` in `bin/camwalld` (the HA select follows).
