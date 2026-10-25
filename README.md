# camwall

A 2x2 wall of four UniFi Protect camera streams on a Raspberry Pi 4, running Raspberry Pi OS Lite (no desktop) and rendering straight to the display through KMS/DRM. Hardware HEVC decode, GStreamer composition, managed by systemd, everything done over SSH. The wall stops itself while the monitor has no power and starts again when it returns.

Written with the help of Claude Sonnet 5.5

> **Highlight: hardware H.265 decoding on a Pi 4.** The four 1280x720 H.265 streams are decoded by the Pi 4's own HEVC block (`rpi-hevc-dec`) and not by the CPU. Stock ffmpeg with `-hwaccel drm` drives it correctly. This is the part that was hard to get working: the obvious GStreamer route (`v4l2slh265dec`) decodes, but its output cannot be displayed correctly on GStreamer 1.26.2, so the wall uses ffmpeg for decoding and GStreamer only for composition and display.
>
> Measured per camera, decode only: about 20% of one CPU core with the hardware decoder, against about 48% in software.
>
> Only the decode runs in hardware. Scaling to 960x540, the conversion to linear frames, the 1080p composition and the copy to the display are CPU work, which limits the wall to about 15-20 fps. An experimental decode-to-plane version (branch `drm-planes`) removes the CPU from the pixel path; see [Design notes](docs/design.md#decode-to-plane-experimental).

```text
camera 1..4 (RTSPS from the UniFi gateway)
   |
   |  one ffmpeg per camera: hardware H.265 decode (rpi-hevc-dec), scale to 960x540, raw I420 frames
   v
four FIFOs
   |
   |  GStreamer: filesrc -> rawvideoparse -> leaky queue -> compositor (1920x1080) -> kmssink
   v
HDMI monitor
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
| Cameras | UniFi G6-series cameras, Encoding set to Enhanced, all send H.265 on RTSP |
| Gateway | UniFi Cloud Gateway running Protect, RTSPS on port 7441 |

A camera on Standard encoding (H.264) also works, but is decoded in software.

## Quick start

Over SSH on the Pi (check the prompt names the Pi first):

1. In UniFi Protect, enable the **Medium** RTSP stream for each camera and note the token (the last path segment of `rtsps://<gateway>:7441/<TOKEN>?enableSrtp`). Keep the cameras' Encoding on **Enhanced** (H.265).
2. Clone the repository and run the installer:

   ```bash
   git clone https://github.com/vihman/camwall.git && cd camwall
   ./install.sh
   ```

   On the first run it creates `/etc/camwall/camwall.conf` from the example and stops.
3. Fill in the gateway and the four tokens, then run the installer again to start the wall:

   ```bash
   sudoedit /etc/camwall/camwall.conf
   ./install.sh
   ```

[`install.sh`](install.sh) is idempotent: run it again after every `git pull` or system update. It installs missing packages, creates the `camwall` user or adds it to missing groups, loads `i2c-dev` for the display check, installs the script and unit only when they differ, never overwrites `camwall.conf`, and restarts the service only when something changed or it is not running. Options: `--dry-run`, `--no-apt`, `--no-restart`.

The manual steps, the hardware checks and how to confirm the hardware decoder is really in use are in [docs/installation.md](docs/installation.md).

## Configuration

Everything lives in `/etc/camwall/camwall.conf` (root:root, 0600, never in the repository). [`config/camwall.conf.example`](config/camwall.conf.example) lists every setting with its default.

| Setting | Default | Meaning |
|---|---|---|
| `UNIFI_HOST` | (required) | Gateway name or IP serving RTSPS on port 7441 |
| `TOKEN1`..`TOKEN4` | (required) | Stream tokens; order is tile position (top-left, top-right, bottom-left, bottom-right) |
| `FPS` | 15 | Frame rate per tile, 1-100 |
| `TILE_WIDTH`, `TILE_HEIGHT` | 960, 540 | One tile; the screen is 2x2 tiles |
| `STREAM_TIMEOUT` | 10 | Seconds without data before a stalled camera restarts the wall |
| `DISPLAY_PROBE` | `ddcutil --syslog never getvcp d6` | Command that succeeds while the monitor is powered; `none` disables the check |
| `PROBE_INTERVAL`, `PROBE_FAILS`, `PROBE_SETTLE` | 5, 3, 3 | Probe period, failures before stopping, delay after the display returns |

## Operation

| Task | Command |
|---|---|
| Status / logs | `systemctl status camwall`, `journalctl -u camwall -f` |
| Restart count | `systemctl show camwall -p NRestarts` |
| Restart / stop | `sudo systemctl restart camwall`, `sudo systemctl stop camwall` |
| Disable autostart | `sudo systemctl disable --now camwall` |
| Change settings or rotate tokens | `sudoedit /etc/camwall/camwall.conf`, then `sudo systemctl restart camwall` |
| Update | `git pull && ./install.sh` |

A dead camera or pipeline makes systemd restart the wall with backoff (3 s up to 60 s); a configuration error exits with status 78 and is not retried. See [docs/operation.md](docs/operation.md) for failure handling, startup checks, display presence and frame rate, and [docs/troubleshooting.md](docs/troubleshooting.md) when something looks wrong.

## Documentation

| File | Contents |
|---|---|
| [docs/installation.md](docs/installation.md) | Step-by-step manual installation, hardware checks, confirming hardware decoding |
| [docs/operation.md](docs/operation.md) | Runtime behaviour: frame rate, failure handling, startup checks, display presence |
| [docs/troubleshooting.md](docs/troubleshooting.md) | Symptoms, causes and fixes |
| [docs/design.md](docs/design.md) | Why ffmpeg + FIFOs, what did not work, the decode-to-plane work, upstream state, planned Home Assistant integration |
| [docs/measurements.md](docs/measurements.md) | CPU, temperature and power measurements on the reference Pi |
| [docs/security.md](docs/security.md) | Token handling, service sandboxing, known exposures |
| [TODO.md](TODO.md) | Open work, by phase |

## Repository layout

```text
bin/camwall                    the wall (installed to /usr/local/bin/camwall)
systemd/camwall.service        the service unit
config/camwall.conf.example    template for /etc/camwall/camwall.conf (secrets live only there)
install.sh                     installs or updates everything, safe to re-run
docs/                          detailed documentation (see above)
TODO.md                        open tasks, by phase
```

Branch `drm-planes` adds `planes/camwall-planes.c`, the experimental decode-to-plane wall.

## Similar projects

At the time of writing (October 2026) no maintained project was found that does a hardware-decoded H.265 multi-camera wall on a 64-bit Raspberry Pi 4. Related work:

| Project | Approach | Notes |
|---|---|---|
| [displaycameras](https://github.com/Anonymousdog/displaycameras) | Shell scripts around omxplayer, aimed at Ubiquiti RTSP | omxplayer does not exist on Bullseye and later or on 64-bit; documented for Buster only |
| [rpisurv](https://github.com/SvenVD/rpisurv) / [OpenSurv](https://github.com/opensurv/opensurv) | rpisurv: omxplayer; OpenSurv: mpv | rpisurv stops at Buster. OpenSurv targets x86 boxes; Pi and H.265 status not stated. UniFi Protect integration requested but not built |
| [Camplayer](https://ipcamtalk.com/threads/camplayer-an-open-source-ip-camera-viewer-grid-video-player-video-wall-for-the-raspberry-pi.49245) | omxplayer / VLC grid | No H.265 in grid mode; inactive |
| [PiPlay](https://github.com/HussX/piplay) | PyQt6 on eglfs, Pi 4+, Bookworm Lite | Lists UniFi cameras and H.265 as tested; whether decoding is in hardware is not stated; no active maintenance planned |
| [denise-video](https://docs.rs/denise-video) | Rust crate: dmabuf to DRM framebuffer to video plane | Closest to the `drm-planes` approach. H.264 verified on Pi; HEVC not finished yet |
| [Frigate](https://docs.frigate.video/configuration/go2rtc) Birdseye, [go2rtc](https://github.com/AlexxIT/go2rtc) | Server composites cameras, restreams one RTSP feed | The Pi would only decode one stream, but it needs a server and adds latency |
| [UniFi Viewport](https://lazyadmin.nl/home-network/unifi-viewport-review/) | Ubiquiti's own HDMI viewer appliance | Turnkey and 4K-capable; reportedly fewer streams in H.265 than in H.264; no custom modes |
| mpv 0.41+ with `v4l2request` | `--hwdec=v4l2request --gpu-hwdec-interop=v4l2request-overlay` | Reported working on Pi HEVC; Debian Trixie ships 0.40 without it |

The state of hardware HEVC support in GStreamer, FFmpeg and the kernel is summarised in [docs/design.md](docs/design.md#upstream-state).

## Security

The camera tokens are credentials. They live only in `/etc/camwall/camwall.conf`; the service runs as an unprivileged, sandboxed account; the RTSPS certificate is not verified, so use camwall only on an internal network. Details in [docs/security.md](docs/security.md).
