# camwall

A 2x2 wall of four camera streams on a Raspberry Pi 4, running Raspberry Pi OS Lite (no desktop) and rendering straight to the display through KMS/DRM. The streams come from IP cameras or recorders that serve RTSP or RTSPS with H.265 or H.264; each tile's URL is set in the configuration. The Pi's hardware decodes the streams and the display controller scales and composes them, so the CPU never touches a pixel. A small daemon under systemd runs the wall, stops it while the monitor has no power and starts it again when the monitor returns. Everything is done over SSH.

Written with the help of Claude Sonnet 5.5

> **Highlight: no CPU in the pixel path.** Each camera's 1280x720 H.265 stream is decoded by the Pi 4's HEVC block (`rpi-hevc-dec`) into frames that stay in the decoder's own tiled memory format. Each frame goes straight onto one of the display controller's overlay planes, and the controller scales and composes them as it scans out. Four cameras at their full 30 fps use about 6% of the CPU (94% idle) and run at about 55 C.
>
> The older design, ffmpeg decode followed by CPU scaling and GStreamer composition, needed about 72% of the CPU at 24 fps and could not reach 30 fps. It is kept as a fallback (`WALL=gstreamer`). See [Measurements](docs/measurements.md) for both.

```text
camera 1..4 (RTSP or RTSPS from the cameras or a recorder)
   |
   |  camwall-planes, one thread per camera: RTSP(S) -> hardware decode
   |  (H.265: rpi-hevc-dec; H.264: the H.264 block)
   |  -> DRM_PRIME frame (NV12, Broadcom SAND tiling), newest frame wins
   v
four overlay planes on the HDMI output: the display controller scales 1280x720 -> 960x540
and composes the 2x2 wall at scanout (atomic KMS)
   |
   v
HDMI monitor

camwalld (systemd, Type=notify, watchdog): starts/stops camwall-planes on the DDC/CI display probe
          and on the mode set over MQTT (optional)
```

## Tested configuration

| Part | Version / detail |
|---|---|
| Board | Raspberry Pi 4 Model B, 2 GB, official 5.1 V / 3 A supply, heatsinks, lid off |
| OS | Raspberry Pi OS Lite (Trixie-based, build 2026-10-06), kernel 6.18.50+rpt-rpi-v8 |
| GStreamer | 1.26.2 (Trixie package) |
| ffmpeg | FFmpeg 7.1 series, Pi OS build (libavformat 61.7) |
| Display | 1920x1080 @ 60 Hz on HDMI-A-1, monitor powered through a smart plug |
| Network | Ethernet, Wi-Fi radio off (measurements before 2026-10-26 were over Wi-Fi) |
| Cameras | UniFi G6-series cameras, Super Encoding on Auto (formerly Encoding: Enhanced), all send H.265 on RTSP |
| Gateway | UniFi Cloud Gateway running Protect, RTSPS on port 7441 |

H.265 is recommended. H.264 streams are decoded on the Pi's separate H.264 block (more than one needs `gpu_mem=256`, see [Installation](docs/installation.md#h264-streams)); the GStreamer wall decodes H.264 in software. Other cameras and recorders than the tested ones should work if they serve such streams, but have not been tried.

## Quick start

Over SSH on the Pi (check the prompt names the Pi first):

1. Find the RTSP or RTSPS URL of each camera's stream, ideally about 1280x720 and H.265. For UniFi Protect, see [Installation](docs/installation.md#unifi-protect).
2. Clone the repository and run the installer:

   ```bash
   git clone https://github.com/vihman/camwall.git && cd camwall
   ./install.sh
   ```

   On the first run it creates `/etc/camwall/camwall.conf` from the example and stops.
3. Fill in the four camera URLs, then run the installer again to start the wall:

   ```bash
   sudoedit /etc/camwall/camwall.conf
   ./install.sh
   ```

[`install.sh`](install.sh) is idempotent: run it again after every `git pull` or system update. It installs missing packages, creates the `camwall` user or adds it to missing groups, loads `i2c-dev` for the display check, builds the plane wall, installs the programs and the unit only when they differ, never overwrites `camwall.conf`, and restarts the service only when something changed or it is not running. Options: `--dry-run`, `--no-apt`, `--no-restart`.

The manual steps, the hardware checks and how to confirm the hardware decoder is really in use are in [docs/installation.md](docs/installation.md).

## Configuration

Everything lives in `/etc/camwall/camwall.conf` (root:root, 0600, never in the repository). [`config/camwall.conf.example`](config/camwall.conf.example) lists every setting with its default.

| Setting | Default | Meaning |
|---|---|---|
| `CAM1_URL`..`CAM4_URL` | (required) | `rtsp://` or `rtsps://` URL of each tile (top-left, top-right, bottom-left, bottom-right). `<NAME>` is replaced with the config variable `NAME` |
| `CAM1_LABEL`..`CAM4_LABEL` | `Camera 1`..`Camera 4` | Camera names shown in MQTT, Home Assistant and the status, e.g. `Gate` |
| `STREAM_HOST`, `STREAM_PORT`, `TOKEN1`.. | (none) | Conventional variables for the URL templates, e.g. `rtsps://<STREAM_HOST>:<STREAM_PORT>/<TOKEN1>`; any name works |
| `WALL` | `planes` | `planes` (decode to display planes) or `gstreamer` (the older wall, as a fallback) |
| `DISPLAY_PROBE` | `ddcutil --syslog never getvcp d6` | Command that succeeds while the monitor is powered; `none` disables the check |
| `PROBE_INTERVAL`, `PROBE_FAILS`, `PROBE_SETTLE` | 5, 3, 3 | Probe period, failures before stopping, delay after the display returns |
| `MQTT` | `no` | `yes`: control and status over MQTT; the `MQTT_*` settings are in [docs/operation.md](docs/operation.md#mqtt) |

The plane wall shows every camera frame and takes its tile size from the display mode. `FPS`, `TILE_WIDTH`, `TILE_HEIGHT` and `STREAM_TIMEOUT` apply only to the GStreamer wall (see [docs/operation.md](docs/operation.md#the-gstreamer-wall-fallback)).

## Operation

| Task | Command |
|---|---|
| Status / logs | `systemctl status camwall`, `journalctl -u camwall -f` |
| Restart count | `systemctl show camwall -p NRestarts` |
| Restart / stop | `sudo systemctl restart camwall`, `sudo systemctl stop camwall` |
| Disable autostart | `sudo systemctl disable --now camwall` |
| Change settings or camera URLs | `sudoedit /etc/camwall/camwall.conf`, then `sudo systemctl restart camwall` |
| Update | `git pull && ./install.sh` |
| Measure CPU, memory and stream backlog | `tools/measure.sh` |
| Check hardware decoding | `tools/check-hwdec.sh` |
| Codec, resolution, bitrate of camera N | `tools/feed-info.sh N` |
| Switch the view remotely | publish `cameras`, `camera1`..`camera4` or a camera label to `camwall/mode/set` over MQTT |
| Check the MQTT settings and connection | `tools/mqtt-test.py` |

A dead camera only blanks its own tile; the wall reconnects it by itself. If the wall process exits, the daemon restarts it with backoff; if the daemon hangs, the systemd watchdog restarts the service. A configuration error exits with status 78 and is not retried. See [docs/operation.md](docs/operation.md) for failure handling, startup checks, display presence and frame rate, and [docs/troubleshooting.md](docs/troubleshooting.md) when something looks wrong.

## Documentation

| File | Contents |
|---|---|
| [docs/installation.md](docs/installation.md) | Step-by-step manual installation, hardware checks, confirming hardware decoding |
| [docs/operation.md](docs/operation.md) | Runtime behaviour: frame rate, failure handling, startup checks, display presence, MQTT |
| [docs/troubleshooting.md](docs/troubleshooting.md) | Symptoms, causes and fixes |
| [docs/design.md](docs/design.md) | The plane wall and the daemon, why the GStreamer wall used ffmpeg + FIFOs, what did not work, upstream state, Home Assistant integration |
| [docs/measurements.md](docs/measurements.md) | CPU, temperature and power measurements on the reference Pi |
| [docs/security.md](docs/security.md) | Token handling, service sandboxing, known exposures |
| [TODO.md](TODO.md) | Open work, by phase |

## Repository layout

```text
bin/camwalld                   the daemon run by the unit (installed to /usr/local/bin/camwalld)
planes/                        the plane wall in C, built by install.sh (/usr/local/bin/camwall-planes):
                                 stream.c (RTSP(S) + decode), display.c (KMS planes), main.c, Makefile
bin/camwall                    the GStreamer wall, the fallback (/usr/local/bin/camwall)
systemd/camwall.service        the service unit
config/camwall.conf.example    template for /etc/camwall/camwall.conf (secrets live only there)
install.sh                     installs or updates everything, safe to re-run
tools/                         measure.sh, check-hwdec.sh, feed-info.sh, mqtt-test.py (see docs/operation.md)
docs/                          detailed documentation (see above)
TODO.md                        open tasks, by phase
```

## Similar projects

At the time of writing (October 2026) no maintained project was found that does a hardware-decoded H.265 multi-camera wall on a 64-bit Raspberry Pi 4. Related work:

| Project | Approach | Notes |
|---|---|---|
| [displaycameras](https://github.com/Anonymousdog/displaycameras) | Shell scripts around omxplayer, aimed at Ubiquiti RTSP | omxplayer does not exist on Bullseye and later or on 64-bit; documented for Buster only |
| [rpisurv](https://github.com/SvenVD/rpisurv) / [OpenSurv](https://github.com/opensurv/opensurv) | rpisurv: omxplayer; OpenSurv: mpv | rpisurv stops at Buster. OpenSurv targets x86 boxes; Pi and H.265 status not stated. UniFi Protect integration requested but not built |
| [Camplayer](https://ipcamtalk.com/threads/camplayer-an-open-source-ip-camera-viewer-grid-video-player-video-wall-for-the-raspberry-pi.49245) | omxplayer / VLC grid | No H.265 in grid mode; inactive |
| [PiPlay](https://github.com/HussX/piplay) | PyQt6 on eglfs, Pi 4+, Bookworm Lite | Lists UniFi cameras and H.265 as tested; whether decoding is in hardware is not stated; no active maintenance planned |
| [denise-video](https://docs.rs/denise-video) | Rust crate: dmabuf to DRM framebuffer to video plane | Same approach as camwall's plane wall. H.264 verified on Pi; HEVC not finished yet |
| [Frigate](https://docs.frigate.video/configuration/go2rtc) Birdseye, [go2rtc](https://github.com/AlexxIT/go2rtc) | Server composites cameras, restreams one RTSP feed | The Pi would only decode one stream, but it needs a server and adds latency |
| [UniFi Viewport](https://lazyadmin.nl/home-network/unifi-viewport-review/) | Ubiquiti's own HDMI viewer appliance | Turnkey and 4K-capable; reportedly fewer streams in H.265 than in H.264; no custom modes |
| mpv 0.41+ with `v4l2request` | `--hwdec=v4l2request --gpu-hwdec-interop=v4l2request-overlay` | Reported working on Pi HEVC; Debian Trixie ships 0.40 without it |

The state of hardware HEVC support in GStreamer, FFmpeg and the kernel is summarised in [docs/design.md](docs/design.md#upstream-state).

## Security

The camera URLs, tokens and passwords are credentials. They live only in `/etc/camwall/camwall.conf`; the service runs as an unprivileged, sandboxed account; the RTSPS certificate is not verified, so use camwall only on an internal network. Details in [docs/security.md](docs/security.md).

## License

MIT, see [LICENSE](LICENSE).
