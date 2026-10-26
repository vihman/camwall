# Design notes and findings

## Why ffmpeg and FIFOs instead of a pure GStreamer pipeline

The Pi 4 decodes HEVC with a stateless V4L2 block (`rpi-hevc-dec`) that outputs Broadcom's column-tiled SAND128 format. GStreamer 1.26.2's `v4l2slh265dec` decodes correctly, but putting its output on the display gave a garbled picture (repeating vertical strips). The decoder advertises `NV12` with the plain `BROADCOM_SAND128` modifier, and the column geometry never reaches the display plane. This is a known upstream gap: GStreamer has no mapping for the Pi column formats yet (see [Upstream state](#upstream-state)). It also could not feed `compositor`.

Stock Pi OS ffmpeg decodes the same streams correctly with `-hwaccel drm` and hands over linear frames, so the production wall uses ffmpeg for decoding and GStreamer only to compose and display linear I420. The price is that scaling, the conversion to linear frames, the composition and the copy to the display are all CPU work (see [Measurements](measurements.md)).

## Things that did not work

| Attempt | Result |
|---|---|
| `v4l2slh265dec ! kmssink` (GStreamer 1.26.2) | Decodes, but the displayed picture is garbled (SAND128 modifier, see above). One early run also failed with "Driver did not accept the bitstream parameters" and a crash; it did not reproduce. |
| mpv from Debian Trixie (0.40) | No `v4l2request` hwdec, falls back to software. |
| Kodi | Hardware decode works, but it shows one video at a time. |
| ffmpeg `-tls_verify 0` | This ffmpeg build rejects the option for RTSP input. The connection works without it, which means the self-signed gateway certificate is not verified. Intentionally left unverified, since camwall is meant for an internal, protected network. |
| `-fflags nobuffer -flags low_delay -map 0:v:0` on ffmpeg | Removed after the tile widths looked wrong, but that turned out to be the 1024x768 fallback mode from booting with the monitor off. Never actually tested on the Pi; retry one at a time if latency matters. |

## Decode-to-plane (experimental)

Branch `drm-planes`, file `planes/camwall-planes.c`. Goal: no CPU in the pixel path. Each camera is decoded by the hardware into DRM_PRIME frames and put on its own display overlay plane; the display controller (HVS) scales and composes at scanout.

Verified on the reference Pi:

- `modetest -M vc4 -p`: HDMI-A-1 uses CRTC 102 (primary plane 91, cursor plane 688). 16 overlay planes (ids 127..292) are available to it. Every plane lists NV12 with BROADCOM_SAND128/64/256 and linear. The CRTC has a writable `BACKGROUND_COLOR` (opaque black).
- `ffmpeg -hwaccel drm -hwaccel_output_format drm_prime -f vout_drm -` shows one stream with a clean picture, so ffmpeg passes the SAND geometry correctly (unlike `v4l2slh265dec ! kmssink`). `-hwaccel_output_format drm_prime` is required, otherwise `vout_drm` rejects the frame.
- `camwall-planes.c` compiles without warnings on the Pi (libdrm-dev 2.4.134, libav*-dev 7.1.5+rpt2 from the Pi archive) and ran with one and then all four cameras, with all four pictures shown and no error lines. (The branch's commit message, "never run on the Pi", predates this run.)

How it works: one thread per camera (libavformat RTSPS over TCP, hardware HEVC decode, DRM_PRIME frames, newest frame wins); the main thread imports frames as DRM framebuffers and commits atomic updates on a 30 fps tick (`WALL_FPS`); a stale or down stream (no frame for 3 s) disables only its plane, so that tile shows the black CRTC background while the others keep running. Each camera reconnects by itself with a growing delay. Tokens come from the environment, never argv. Until it has its own unit, test it as a transient unit with the service stopped:

```bash
gcc -O2 -Wall -o camwall-planes planes/camwall-planes.c \
  $(pkg-config --cflags --libs libdrm libavformat libavcodec libavutil) -lpthread
sudo install -m 0755 camwall-planes /usr/local/bin/camwall-planes
sudo systemctl stop camwall
sudo systemd-run --unit=planes-test -p User=camwall -p SupplementaryGroups="video render" \
  -p EnvironmentFile=/etc/camwall/camwall.conf /usr/local/bin/camwall-planes
journalctl -u planes-test -f
# back to the GStreamer wall:
sudo systemctl stop planes-test; sudo systemctl start camwall
```

Building needs `libdrm-dev libavformat-dev libavcodec-dev libavutil-dev gcc pkg-config`.

Tested on the Pi (2026-10-26, run as user `camwall`):

- **Dead camera:** one camera restarted from Protect. Only its tile went black; the other three kept running. While the camera was down, the program retried about every 12 s (10 s connection timeout plus up to 5 s backoff), logging `Connection timed out`. Once the camera was back, the stream and the tile returned by themselves, with no restart of the program.
- **Monitor power cut and restore** (smart plug, about 30 s): the picture was back as soon as the monitor came up, with nothing in the journal. The HDMI output keeps running while the monitor is unpowered, so the program sees no interruption and needs no re-modeset. It keeps decoding while the monitor is off; stopping that is the job of the display-presence check.
- **Device nodes in use:**
  - `/dev/dri/card1` (display)
  - `/dev/video19` (HEVC decoder)
  - `/dev/media1` (the decoder's media node; its number varies between boots)
  - `/dev/dma_heap/linux,cma` (frame buffers)
  
  No render node. A production unit should therefore allow device classes (`DeviceAllow=char-drm`, `char-video4linux`, `char-media`, `char-dma_heap`, plus `char-i2c` for the DDC probe) rather than node numbers. Check the class names against `/proc/devices`.

Review findings and weak spots:

- **Missing compared with the GStreamer wall:** no display-presence probe (it keeps decoding with the monitor off), no config validation, no tile settings, no decoder check, no systemd notify or watchdog, no per-camera health output for a supervisor.
- **Root is not needed** (verified 2026-10-26). Run as user `camwall` with only the groups `video` and `render`, it gets the display and streams all four cameras. Like `kmssink`, it becomes DRM master because it is the first to open the card, which needs no extra capability. Still to test: running it with the full hardening of `camwall.service`.
- **Monitor power cycle:** works (see above). The flip-timeout re-modeset path was not triggered and remains unexercised. Presence detection still needs DDC/CI.
- **Guesses that happened to work or are unexercised:** the first modeset turns the primary plane off (the fallback, a black 1920x1080 dumb buffer, is untested); the DRM device path passed to FFmpeg's hwdevice creation; frame size and modifier taken from FFmpeg's descriptor. H.264 cameras are refused ("set Encoding to Enhanced").
- **Smaller points:** it decodes every frame at the camera rate (fine in hardware, but each held frame uses CMA memory; four streams should fit, about seven is the reported Pi 4 limit); it creates one hardware device context per camera where one shared context would do.

### Language split

Python with ctypes and hand-written FFmpeg struct offsets was judged too fragile. A Python+cffi rewrite (API mode, compiled against the real headers, with a small C glue block for the interrupt callback, `get_format`, the ioctls and the DRM event handler) was considered. The chosen split is to keep the C program as a thin pixel path, run as a child process, under a Python control daemon (stdlib only, no pip). Frames never pass through Python, and a native crash is just a child's exit code.

## Display presence detection

Findings (2026-10-14, Pi 4, vc4-kms, monitor on a smart plug on HDMI-A-1):

- HDMI connector status cannot detect the monitor power being cut: `status` stays `connected`, and a forced probe (`modetest -M vc4 -c`) still reports `connected` with the EDID (530x300 mm), even when the Pi booted with the monitor unpowered. The EDID is evidently served from the Pi's 5 V. Boot config has no hotplug overrides, so that is not the cause.
- DDC/CI works as a presence probe: `ddcutil getvcp d6` prints `Display not found` when the monitor is unpowered and `DPM: On` when it is on. It needs the `i2c-dev` module.
- Timing (one probe every ~2 s plus call time): failures start within ~3 s of the cut; 6 consecutive failures over ~22 s while off; success again once the monitor was up; no failures in 12 calls with the monitor on (small sample).
- The monitor answers DDC for features it does not support (`Feature 0xdd should not exist but ... succeeds`); `ddcutil` copes with it and `getvcp d6` still gives a real answer, but it probably adds a second or two per probe.
- Probe cost, measured while idle (monitor unpowered): 1272 ms CPU in 60 s, about 2% of one core. If it ever matters, raise `PROBE_INTERVAL` rather than adding `--bus`.
- Booting the Pi with the monitor off leaves the console at the 1024x768 fallback mode, and `kmssink` then crops the picture. Accepted as a corner case (see [Troubleshooting](troubleshooting.md)).

In production since 2026-10-15: a plug cut stops the wall (no ffmpeg or GStreamer process left, FIFOs removed, `kmssink` releases the display) and restoring it brings the picture back; a boot with the monitor off waits for it.

## Upstream state

Researched 2026-10; recheck before relying on any of it.

- **GStreamer:** Pi column-format (SAND/NC12) support for `v4l2codecs` is GStreamer MR !9247, still open in early 2026 ([discussion](https://discourse.gstreamer.org/t/v4l2codecs-feasibility-of-adding-support-for-raspberry-pi-hevc-sand-formats-nc12-nc30/3522), [raspberrypi/linux#7137](https://github.com/raspberrypi/linux/issues/7137)). Once it lands, `v4l2slh265dec ! kmssink` per plane may become possible without custom C.
- **FFmpeg:** upstream has no V4L2 request (stateless) decoding yet; the Pi relies on the Raspberry Pi fork shipped in Pi OS (`--enable-v4l2-request --enable-sand --enable-vout-drm`). Anything built on libav here depends on Pi OS's ffmpeg packages.
- **Kernel:** the `rpi-hevc-dec` driver was being upstreamed (series at v6 in 2026-03, [Phoronix](https://www.phoronix.com/news/Raspberry-Pi-HEVC-H265-Decode)).
- **mpv:** 0.41 with a v4l2request patch reports `--hwdec=v4l2request --gpu-hwdec-interop=v4l2request-overlay` working on the Pi 6.18 kernel. Trixie ships 0.40, which does not have it.
- **Memory:** a Pi 4 tops out around seven concurrent HEVC streams because of CMA ([info-beamer](https://community.info-beamer.com/t/pi4-troubles-with-7-streams/1338)). Watch `CmaFree` in `/proc/meminfo`.
- **Pi 5:** same HEVC driver and SAND format, faster CPU, no H.264 hardware decoder (irrelevant for H.265 cameras). The current CPU-scaling design would probably reach 30 fps there unchanged (inferred, not measured).
- **UniFi RTSPS:** [go2rtc#2071](https://github.com/AlexxIT/go2rtc/issues/2071) reports port 7441 RTSPS failures where `rtsp://<gateway>:7447/<token>?enablertsp` works. Not official; not seen here, but a known fallback.
- **uiprotect** (Python, packaged as `python3-uiprotect` in Debian) can create and fetch RTSPS stream URLs through the Protect API ([docs](https://uiprotect.readthedocs.io/en/latest/api/)), which could automate token rotation.

## Planned: Home Assistant integration

Not built; see [TODO](../TODO.md) phase 3.

- The remote button is an IKEA BILRESA scroll wheel, a Matter-over-Thread device paired with Home Assistant. It appears in HA as 9 Matter event entities, **disabled by default**: 3 channels, each with clockwise, anticlockwise and press. Channel switching happens on the device. A short press is reported only after the ~1 s double-press timeout, and the wheel is reported flaky, so use fixed button presses to set a mode, not the wheel. Some devices reportedly "fall asleep" until the batteries are reseated. Automations must trigger on event-entity state changes; no device triggers appear ([community thread](https://community.home-assistant.io/t/matter-ikea-bilresa-scrollwheel-media-control/1013472)).
- Control path: MQTT, through the broker Home Assistant already uses. The daemon publishes an HA MQTT-discovery `select` entity (`cameras`, `info`, `off`, later single-camera) plus availability and health. HA automations set fixed select targets from the BILRESA events rather than a blind toggle. The Pi gets its own Mosquitto user, credentials in `/etc/camwall/` (0600). Python client: `python3-paho-mqtt` from apt.
- Info mode: a Chromium kiosk under `cage` (Wayland, DRM direct) showing an HA dashboard, started and stopped by the daemon and mutually exclusive with the cameras (both need DRM master). Auth via a long-lived token for a dedicated non-admin HA user, or `trusted_networks` limited to the Pi's IP. Unverified: injecting the token into the browser storage via the DevTools protocol.
- Daemon: one `camwalld.service` (`Type=notify`, watchdog, `Restart=always`). The daemon is the control plane only; the wall and the browser run as child processes.

## Ideas not built

- Hardware H.264 for cameras on Standard: ffmpeg's `h264_v4l2m2m` (picking the decoder per camera via `ffprobe`), or a GStreamer wall with `v4l2h264dec`. Neither tried.
- With the GStreamer wall: raising the FIFO pipe size (`F_SETPIPE_SZ`, capped by `/proc/sys/fs/pipe-max-size`) or using Unix sockets to cut the `filesrc` overhead; black fallback tiles via `videotestsrc` and the compositor's `ignore-inactive-pads`. Only worth doing if the decode-to-plane wall is abandoned.
