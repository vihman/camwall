# camwall

A camera wall for the Raspberry Pi 4: four RTSP or RTSPS camera streams on one HDMI monitor, as a 2x2 grid or one camera full screen. It runs on Raspberry Pi OS Lite with no desktop and is managed over SSH.

Written with the help of Claude Sonnet 5.5

## Features

- **Hardware decoding straight to the display.** The Pi 4's hardware video decoders, dedicated circuits in its chip, decode H.265 and H.264, and each decoded frame goes directly onto a display plane. The display controller scales and composes the wall as it scans out, so the CPU never touches a pixel. Four cameras at their full frame rate leave the CPU almost idle, where the older software design could not keep up ([measurements](docs/measurements.md)).
- **Grid or single camera.** Switch between the 2x2 grid and any one camera full screen instantly, without reconnecting.
- **Any RTSP/RTSPS source** that sends H.265 or H.264: IP cameras or a recorder. One URL per tile, with shared parts such as host, port or token written once.
- **Follows the monitor.** While the monitor has no power (detected over DDC/CI), nothing is decoded or streamed; the wall returns with the monitor.
- **Optional remote control over MQTT**, plain or TLS. With Home Assistant, the wall appears automatically as a view selector, a status sensor and a display sensor, so HA automations and buttons can switch the view. Any other broker works too: publish the view to a topic and read the state back.
- **Optional dashboard view.** An image, such as a Home Assistant dashboard rendered on the HA machine, shown full screen in place of the cameras and refreshed periodically. No browser runs on the Pi.
- **Runs unattended.** A dead camera blanks only its own tile and reconnects by itself. A small daemon supervises the wall under systemd, with a watchdog, as an unprivileged, sandboxed user.

## Tested with

| Part | Detail |
|---|---|
| Board | Raspberry Pi 4 Model B, 2 GB, official power supply, heatsinks |
| OS | Raspberry Pi OS Lite (Trixie), kernel 6.18, the Pi OS FFmpeg 7.1 build (upstream FFmpeg lacks the Pi's hardware decoding) |
| Display | 1920x1080 HDMI monitor |
| Cameras | UniFi Protect (G6 series), H.265 at 1280x720, one also at 3840x2160; H.264 tested as well |

Other cameras and recorders should work if they send H.265 or H.264 over RTSP, but have not been tried.

Resolution is limited by the decoders, not the CPU. The HEVC decoder handles about one 4K stream at 60 fps in total, so one 4K camera fits next to three 720p ones (measured), four cameras at 1080p should fit (not measured), but four 4K cameras (twice the decoder's capacity) do not. The H.264 decoder stops at about 1080p per stream. Details in [measurements](docs/measurements.md#a-4k-camera-in-the-plane-wall). More than one H.264 camera needs `gpu_mem=256` ([installation](docs/installation.md#h264-streams)).

## Quick start

On the Pi, over SSH:

1. Find each camera's RTSP or RTSPS URL, ideally about 1280x720 and H.265 ([UniFi Protect](docs/installation.md#unifi-protect)).
2. Install. The first run creates `/etc/camwall/camwall.conf` from the example and stops:

   ```bash
   git clone https://github.com/vihman/camwall.git && cd camwall
   ./install.sh
   ```

3. Fill in the four camera URLs, then start the wall:

   ```bash
   sudoedit /etc/camwall/camwall.conf
   ./install.sh
   ```

`install.sh` is safe to re-run after every `git pull` or system update. It installs packages, builds the plane wall, installs the service and never overwrites your configuration. Manual installation and hardware checks are in [docs/installation.md](docs/installation.md).

## Configuration

`/etc/camwall/camwall.conf` (root-only; every setting is in [`config/camwall.conf.example`](config/camwall.conf.example)):

| Setting | Default | Meaning |
|---|---|---|
| `CAM1_URL`..`CAM4_URL` | (required) | Stream URL per tile (top-left, top-right, bottom-left, bottom-right). `<NAME>` is replaced with the config variable `NAME`, e.g. `rtsps://<STREAM_HOST>:<STREAM_PORT>/<TOKEN1>` |
| `CAM1_LABEL`..`CAM4_LABEL` | `Camera 1`.. | Camera names for MQTT, Home Assistant and the status |
| `DISPLAY_PROBE` | `ddcutil ... getvcp d6` | Command that succeeds while the monitor is on; `none` disables the check |
| `MQTT` | `no` | `yes` enables MQTT control; settings in [docs/operation.md](docs/operation.md#mqtt) |
| `INFO_URL` | (none) | Image URL for the dashboard view; settings in [docs/operation.md](docs/operation.md#dashboard-info-mode) |
| `WALL` | `planes` | `gstreamer` selects the older fallback wall |

## Operation

| Task | Command |
|---|---|
| Status / logs | `systemctl status camwall`, `journalctl -u camwall -f` |
| Restart / stop | `sudo systemctl restart camwall`, `sudo systemctl stop camwall` |
| Apply changed settings | `sudoedit /etc/camwall/camwall.conf`, then `sudo systemctl restart camwall` |
| Update | `git pull && ./install.sh` |
| Switch the view | publish `cameras`, `camera1`..`camera4` or a camera label to `camwall/mode/set` |
| Measure CPU, memory, streams | `tools/measure.sh` |
| Check hardware decoding | `tools/check-hwdec.sh` |
| Codec, resolution, bitrate of camera N | `tools/feed-info.sh N` |
| Check MQTT | `tools/mqtt-test.py` |

## Documentation

| File | Contents |
|---|---|
| [docs/installation.md](docs/installation.md) | Manual installation, stream URLs, H.264, hardware checks |
| [docs/operation.md](docs/operation.md) | Display presence, failure handling, MQTT and Home Assistant, the fallback wall |
| [docs/troubleshooting.md](docs/troubleshooting.md) | Symptoms, causes and fixes |
| [docs/design.md](docs/design.md) | How it works, repository layout, what did not work, upstream state |
| [docs/measurements.md](docs/measurements.md) | CPU, memory, temperature and power on the reference Pi |
| [docs/security.md](docs/security.md) | Credentials, sandboxing, known exposures |
| [TODO.md](TODO.md) | Open work |

## Similar projects

As of October 2026, no maintained project was found that decodes several H.265 cameras in hardware on a 64-bit Raspberry Pi 4.

| Project | Approach | Notes |
|---|---|---|
| [displaycameras](https://github.com/Anonymousdog/displaycameras) | omxplayer scripts | omxplayer is gone since Bullseye and on 64-bit |
| [rpisurv](https://github.com/SvenVD/rpisurv) / [OpenSurv](https://github.com/opensurv/opensurv) | omxplayer / mpv | rpisurv stops at Buster; OpenSurv targets x86 |
| [Camplayer](https://ipcamtalk.com/threads/camplayer-an-open-source-ip-camera-viewer-grid-video-player-video-wall-for-the-raspberry-pi.49245) | omxplayer / VLC grid | No H.265 in grid mode; inactive |
| [PiPlay](https://github.com/HussX/piplay) | PyQt6 on eglfs | H.265 listed; hardware decoding not stated; unmaintained |
| [denise-video](https://docs.rs/denise-video) | Rust: dmabuf to DRM plane | Same approach; HEVC not finished |
| [Frigate](https://docs.frigate.video/configuration/go2rtc) Birdseye, [go2rtc](https://github.com/AlexxIT/go2rtc) | Server composites, restreams one feed | Needs a server, adds latency |
| [UniFi Viewport](https://lazyadmin.nl/home-network/unifi-viewport-review/) | Ubiquiti's HDMI viewer | Turnkey; UniFi only; no custom modes |
| mpv 0.41+ (`--hwdec=v4l2request`) | Player | Reported working on Pi HEVC; Trixie ships 0.40 |

## Security

Stream URLs, tokens and passwords are credentials and live only in the root-only config file. The service runs unprivileged and sandboxed. RTSPS certificates are not verified, so use camwall on an internal network only. See [docs/security.md](docs/security.md).

## License

MIT, see [LICENSE](LICENSE).
