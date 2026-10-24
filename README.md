# camwall

A 2x2 wall of four UniFi Protect camera streams on a Raspberry Pi 4, running Raspberry Pi OS Lite (no desktop) and rendering straight to the display through KMS/DRM. Hardware HEVC decode, GStreamer composition, managed by systemd, everything done over SSH.
Written with the help of Claude Sonnet 5.5

> **Highlight: hardware H.265 decoding on a Pi 4.** The four 1280x720 H.265 streams are decoded by the Pi 4's own HEVC block (`rpi-hevc-dec`) and not by the CPU. Stock ffmpeg with `-hwaccel drm` drives it correctly. This is the part that was hard to get working: the obvious GStreamer route (`v4l2slh265dec`) decodes, but its output cannot be displayed correctly on GStreamer 1.26.2, so the wall uses ffmpeg for decoding and GStreamer only for composition and display.
>
> Measured per camera, decode only: about 20% of one CPU core with the hardware decoder, against about 48% in software. See [Confirming the hardware decoder is in use](#confirming-the-hardware-decoder-is-in-use) for how to check it on your own Pi.
>
> Only the decode runs in hardware. Scaling to 960x540, the conversion to linear frames, the 1080p composition and the copy to the display are CPU work.

```
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
| ffmpeg | libavformat 61.7 (FFmpeg 7.1 series, Pi OS build) |
| Display | 1920x1080 @ 60 Hz on HDMI-A-1, monitor powered through a smart plug |
| Network | Wi-Fi (brcmfmac); the Ethernet port was not connected. The 30 fps measurement below shows the Wi-Fi driver among the busiest threads |
| Cameras | UniFi G6 series (Bullet, Turret and Pro Turret models tested); all with Encoding set to Enhanced, all send H.265 on RTSP |
| Gateway | UniFi Cloud Gateway running Protect, RTSPS on port 7441 |

The default setup uses H.265 streams (Protect's Enhanced encoding). A mixed H.264/H.265 setup was also tested; H.264 decoding used software (see [Measurements](#measurements)).

## Repository layout

```
bin/camwall                    the wall (installed to /usr/local/bin/camwall)
systemd/camwall.service        the service unit
config/camwall.conf.example     template for /etc/camwall/camwall.conf (secrets live only there)
install.sh                     installs or updates everything below, safe to re-run
TODO.md                        open tasks and findings
```

## Installation

Do all of this over SSH. Check the prompt names the Pi before running any command, because a dropped SSH session silently leaves you in your local shell and the commands then run on the wrong machine.

### Quick install and update

[`install.sh`](install.sh) does steps 2, 4, 5 (only creating the file) and 6 below, plus the `i2c-dev` setup for the display check. Run it after a fresh clone and again after every `git pull` or system update:

```bash
./install.sh
```

It installs missing packages, creates the `camwall` user or adds it to missing groups, installs the script and unit only when they differ, never overwrites `/etc/camwall/camwall.conf`, and restarts the service only when something changed or it is not running. On the first run it creates `camwall.conf` from the example and stops so you can fill in the host and tokens (`sudoedit /etc/camwall/camwall.conf`); run it again to start the wall. Options: `--dry-run` shows what it would do without changing anything, `--no-apt` skips package installation, `--no-restart` leaves a running service alone. Steps 1 and 3 (Protect setup and hardware checks) stay manual. The numbered steps below are the same thing done by hand.

### 1. Enable RTSP streams in UniFi Protect

For each camera, open its settings in Protect, go to the RTSP section, and enable the **Medium** stream (1280x720). The UI shows a URL like `rtsps://<gateway>:7441/<TOKEN>?enableSrtp`. The token is the last path segment. Medium suits the 960x540 tiles; Low (640x360) would have to be upscaled.

Treat the token as a credential: anyone on the network who has it can watch that camera. Regenerate it in Protect if it ever leaks.

**Leave the camera's video encoding on Enhanced.** In Protect the setting is under the camera's Recording Settings, Recording Quality, Encoding, and offers Standard, Enhanced and Advanced. In UniFi's wording Enhanced is H.265 (HEVC) and Standard is H.264 (a third-party Protect review describes it that way). The setting sits in the Recording Quality block, so Protect presents it as a recording setting, but it also governs the RTSP streams. On the reference system all four cameras were on Enhanced and all four streams came out as H.265, which is what this wall is built around. Switching one camera to Standard changed its Medium RTSP stream to H.264 (1280x720, 30 fps); that was tested on one camera. What Advanced does is not known. If you switch to Standard, check the codec with `ffmpeg -hide_banner -rtsp_transport tcp -i "rtsps://<gateway>:7441/<TOKEN>"` before relying on the wall. A camera on Standard still works, but its H.264 stream is decoded in software (see Measurements), so it costs more CPU than an Enhanced camera.

### 2. Install packages

```bash
sudo apt update
sudo apt install -y ffmpeg gstreamer1.0-tools gstreamer1.0-plugins-base \
  gstreamer1.0-plugins-good gstreamer1.0-plugins-bad v4l-utils libdrm-tests ddcutil
```

`v4l-utils` and `libdrm-tests` are only for the checks in the next step.

### 3. Check the hardware

```bash
# HEVC hardware decoder: expect rpi-hevc-dec with /dev/video19 and /dev/media0
v4l2-ctl --list-devices | grep -A2 rpi-hevc-dec

# Display: expect a connected HDMI connector and a 1920x1080 mode marked "preferred"
modetest -M vc4 -c | grep -E 'connected|preferred'
```

If the monitor's preferred mode is not 1920x1080, set `TILE_WIDTH` and `TILE_HEIGHT` in the configuration (step 5) to half of the mode's width and height, for example `640` and `360` for 1280x720. Both must be even. The wall is 2x2 tiles, so the screen size is derived from them.

### 4. Create the service account

A dedicated system user with no login and no home directory, in the groups needed for the display, the decoder and the display check (`i2c`). The unit gives it a working `HOME` of `/var/lib/camwall` (`StateDirectory=camwall`, `Environment=HOME=...`), where GStreamer keeps its plugin registry cache and `ddcutil` its cache under `~/.cache`; with the read-only filesystem sandbox this and `/run/camwall` are the only places the service can write:

```bash
sudo useradd --system --no-create-home --shell /usr/sbin/nologin --groups video,render,i2c camwall
```

The wall stops itself while the monitor has no power (for example a smart plug cut), using DDC/CI over HDMI. That needs the `i2c-dev` kernel module, loaded now and at every boot:

```bash
echo i2c-dev | sudo tee /etc/modules-load.d/i2c.conf
sudo modprobe i2c-dev
```

Check that it works with the monitor on and off: `ddcutil getvcp d6` should print `DPM: On` with the monitor powered and `Display not found` without. The HDMI connector status cannot be used for this, because it keeps reporting `connected` (the EDID is served from the Pi's 5 V). If your monitor does not answer DDC/CI, set `DISPLAY_PROBE=none` in the configuration below.

### 5. Install the configuration

```bash
sudo install -d -m 0755 /etc/camwall
sudo install -m 0600 -o root -g root config/camwall.conf.example /etc/camwall/camwall.conf
sudoedit /etc/camwall/camwall.conf      # set UNIFI_HOST and TOKEN1..TOKEN4
sudo ls -l /etc/camwall/camwall.conf    # expect -rw------- root root
```

The file holds the gateway, the four tokens and all optional settings (frame rate, tile size, stream timeout, display check), each commented-out with its default in the example. systemd reads it as root before it drops to the `camwall` user, so the service account never needs read access to it. The format is plain `KEY=value` lines, and comments must be on their own lines (systemd does not accept trailing comments).

Upgrading from a version that used `/etc/camwall/tokens.env`: `install.sh` moves the file to `camwall.conf` for you. By hand, run `sudo mv /etc/camwall/tokens.env /etc/camwall/camwall.conf` before installing the new unit, otherwise the service cannot start.

### 6. Install the script and the unit, then start

```bash
sudo install -m 0755 -o root -g root bin/camwall /usr/local/bin/camwall
sudo install -m 0644 -o root -g root systemd/camwall.service /etc/systemd/system/camwall.service
sudo systemctl daemon-reload
sudo systemctl enable --now camwall
```

The wall should appear within a few seconds.

### 7. Verify

```bash
systemctl status camwall --no-pager
journalctl -u camwall -f              # Ctrl-C leaves the service running
vcgencmd measure_clock arm            # ~1500 MHz under load means no throttling
vcgencmd get_throttled                # 0x0 means no under-voltage or thermal throttling
vcgencmd measure_temp
```

Then reboot once (`sudo reboot`) and confirm the wall comes up without anybody logging in. Also switch the monitor off and on with the service running and check that the picture comes back.

### Confirming the hardware decoder is in use

ffmpeg's `-hwaccel` option can fall back to software decoding silently, so check it instead of assuming. On the reference Pi, with the wall running as the `camwall` service, the hardware decoder was confirmed in use:

- **Decoder device held open.** `fuser -v /dev/video19 /dev/media0` listed all four ffmpeg processes on both nodes. A later check on the same Pi listed them on `/dev/video19` only; that is the node the script's own check looks at, so `/dev/media0` is not required.
- **Decoder interrupts.** The interrupt counter of the HEVC decoder (IRQ `feb00000.codec`) rose by about 1200 in 5 seconds (roughly 240 per second) while the wall decoded 4 cameras at 15 fps, which is 60 frames per second in total.
- **CPU cost.** Four concurrent streams cost about 1.9-2.0 s of CPU time per camera over a ~9.5 s run with `-hwaccel drm`, against 4.3-4.9 s without it.
- **Kernel driver message.** `dmesg` showed a line from the decoder's kernel driver (`rpi-hevc-dec feb00000.codec: Missing inuse DPB ent 0, timestamp=0`) at the start of the hardware-decode runs that were checked, and not during software-decode runs.

To repeat the checks on your own Pi while the wall (or any `-hwaccel drm` ffmpeg) is running:

```bash
# 1. The decoder device nodes should be held open by the ffmpeg processes
sudo fuser -v /dev/video19 /dev/media0

# 2. The decoder's interrupt counter should rise between two samples.
#    The IRQ is named after the device in dmesg (feb00000.codec on the reference Pi).
grep -iE 'hevc|rpivid|feb00000' /proc/interrupts; sleep 5; grep -iE 'hevc|rpivid|feb00000' /proc/interrupts

# 3. A direct A/B on one stream: CPU seconds with and without the hardware decoder
for opt in "-hwaccel drm" ""; do
  echo "== ffmpeg $opt"
  ffmpeg -nostdin -hide_banner -loglevel info -benchmark -rtsp_transport tcp $opt \
    -i "rtsps://$UNIFI_HOST:7441/$TOKEN1" -an -t 10 -f null - 2>&1 | tr '\r' '\n' | grep 'bench: utime'
done
```

The device numbers (`/dev/video19`, `/dev/media0`) are the ones `v4l2-ctl --list-devices` reports for `rpi-hevc-dec` on the reference Pi; they can differ on yours. For check 3, export `UNIFI_HOST` and `TOKEN1` into your shell first (the env file is root-only, so type them in; avoid leaving a token in your shell history). The `utime + stime` of the hardware run should be well below the software run.

## Operation

| Task | Command |
|---|---|
| Status / logs | `systemctl status camwall`, `journalctl -u camwall -f` |
| Restart count | `systemctl show camwall -p NRestarts` |
| Restart / stop | `sudo systemctl restart camwall`, `sudo systemctl stop camwall` |
| Disable autostart | `sudo systemctl disable --now camwall` |
| Change frame rate | see [Changing the frame rate](#changing-the-frame-rate) |
| Rotate tokens | `sudoedit /etc/camwall/camwall.conf`, then `sudo systemctl restart camwall` |
| Update the script and unit | `git pull && ./install.sh` (restarts the service if anything changed) |

Under systemd the FIFOs live in `/run/camwall` (`RuntimeDirectory=`, removed when the service stops). Run by hand, the script falls back to a temporary directory. Do not run `camwall` by hand while the service is running. Two copies fight over the display and the camera connections. Stop the service first.

ffmpeg errors in the journal carry a `camN:` prefix, which tells you which camera failed.

### Changing the frame rate

`FPS` is read from the service's environment and defaults to 15. Set it in `/etc/camwall/camwall.conf` (the example file has a commented-out `#FPS=15` line) and restart the service:

```bash
sudoedit /etc/camwall/camwall.conf      # FPS=20
sudo systemctl restart camwall
```

It must be a whole number from 1 to 100; anything else stops the service with a configuration error (status 78). If an older install still has an `FPS` drop-in in `/etc/systemd/system/camwall.service.d/`, remove it, because two places setting the same variable is confusing.

The default of 15 fps is the conservative choice. 20 fps was tested, but check load and latency on your own Pi. At 30 fps the reference Pi overloaded and the picture lagged (see [Measurements](#measurements)).

Check that it took effect. The command prints only the `fps=` part of each ffmpeg command line, so no token is shown; expect one line, `4 fps=20`. It checks every ffmpeg process on the machine, so the count assumes no other ffmpeg processes are running:

```bash
ps -o args= -p "$(pgrep -d, -x ffmpeg)" | grep -oE 'fps=[0-9]+' | sort | uniq -c
```

### How failures are handled

The script runs the wall once and exits as soon as any ffmpeg process or the GStreamer pipeline dies. systemd restarts it after 3 seconds, with no limit on retries. Repeated failures back off: the delay grows in four steps from 3 s to a maximum of 60 s (`RestartSteps=`, `RestartMaxDelaySec=`), so a broken gateway or network is not hammered. A camera that goes silent without closing its connection is caught by ffmpeg's socket timeout (`STREAM_TIMEOUT`, default 10 s, passed as `-timeout`): ffmpeg exits and the wall restarts like for any other dead camera. A configuration error (missing `UNIFI_HOST` or token, invalid `FPS` or `PROBE_*` value) exits with status 78 and is not restarted at all; the journal says what to fix, and after fixing it run `sudo systemctl restart camwall`. The compositor waits for all four inputs, so one camera that stays offline keeps the screen blank until it returns. Showing black tiles for dead cameras would need a different design and has not been built.

### Startup checks

At start the script checks that `ffmpeg` and `gst-launch-1.0` exist (fatal if not) and warns in the journal if no `rpi-hevc-dec` V4L2 device is found, if the service user cannot open it, or if this ffmpeg has no `drm` hwaccel; in those cases H.265 is decoded in software. About 10 s after the wall starts it also checks that each ffmpeg holds the decoder device open and logs `camN: ffmpeg does not hold ...` for one that does not, which is expected for a camera on H.264 (Standard). These are warnings only. They automate the `fuser` check in [Confirming the hardware decoder is in use](#confirming-the-hardware-decoder-is-in-use).

### Display presence

The wall also depends on the monitor being powered. While it runs, the script probes the display every `PROBE_INTERVAL` seconds (default 5) with `DISPLAY_PROBE` (default `ddcutil --syslog never getvcp d6`; `--syslog never` keeps ddcutil's own messages out of the journal). After `PROBE_FAILS` consecutive failures (default 3, so about 15 s) it stops all ffmpeg processes and the pipeline, which also ends the RTSP sessions, and idles, probing until the monitor answers again. It then waits `PROBE_SETTLE` seconds and starts the wall. The service stays `active` while idle; the journal shows `no display detected, idle until it returns` and `display detected, starting the wall`. If the Pi boots with the monitor off, the wall simply starts later. This replaces nothing in the failure handling above: a dead camera still exits the script and systemd restarts it. If the probe command is not installed, the script logs that and runs without the check; `DISPLAY_PROBE=none` turns it off explicitly. All four settings are optional and go in `/etc/camwall/camwall.conf`.

## Troubleshooting

| Symptom | Likely cause and check |
|---|---|
| Picture shows old timestamps, minutes behind | The leaky queues drop frames when full, but do not bound latency across the whole pipeline, which can still fall behind. Check `vcgencmd get_throttled` and the clock first; an under-voltage or overheating Pi runs at a fraction of its speed. Check the load with `top -b -n 2 -d 3 -H -w 200`. Lower `FPS`. |
| `Undervoltage detected!` in `dmesg`, `throttled` bits 0 or 2 set | Weak supply or thin/long cable. Use the official 5.1 V / 3 A supply and a short, thick cable. Bits 16 and above are since-boot history and clear on reboot. |
| Temperature near 80 C, clock dropping | Cooling. Add heatsinks or a fan, take the lid off. |
| Heavy CPU use by `kworker ... brcmf_wq` | The Pi is receiving the streams over Wi-Fi. Use Ethernet (`ip route get <gateway ip>` shows the interface). |
| `Connection timed out` or `Operation timed out` from one camera | The stream stalled for longer than `STREAM_TIMEOUT` (default 10 s) and ffmpeg gave up; the wall restarts. If it repeats on a healthy network, raise `STREAM_TIMEOUT` in `/etc/camwall/camwall.conf`. |
| `Connection reset by peer`, `Error in the pull function` from one camera | That camera's session was dropped. The wall restarts itself. If it repeats, check for a saturated Pi or an unstable network path. |
| `Could not find ref with POC ...` at start | ffmpeg joined the stream between keyframes. Normal at startup. |
| `rpi-hevc-dec ... Missing inuse DPB ent 0` once at startup | Normal at startup. Worry only if it repeats during steady playback. |
| DRM permission or "master" errors in the journal | The service user must be in `video` and `render` (and `i2c` for the display check). Check `id camwall`. Make sure no other process holds the display. |
| Screen goes black after about 10 minutes while the service is active | Console blanking. Add `consoleblank=0` to the single line in `/boot/firmware/cmdline.txt` (keep it on one line) and reboot. |
| Journal says `preflight:` or `ffmpeg does not hold` | The hardware decoder is not in use (see Startup checks). Check `v4l2-ctl --list-devices`, `id camwall` (groups `video`, `render`) and `ffmpeg -hwaccels`. Expected for a camera on H.264. |
| Journal says `no display detected` while the monitor is on | `ddcutil getvcp d6` fails as the `camwall` user. Check `i2c-dev` is loaded (`lsmod \| grep i2c_dev`), `id camwall` shows `i2c`, and the monitor supports DDC/CI (some need it enabled in their on-screen menu). Otherwise set `DISPLAY_PROBE=none`. |
| Tiles look cropped or have different widths after booting the Pi with the monitor off | The console came up at the 1024x768 fallback mode and `kmssink` crops and scales to it (`sudo cat /sys/kernel/debug/dri/1/state` shows the CRTC at 1024x768 instead of 1920x1080). Reboot with the monitor powered on. To avoid it, add `video=HDMI-A-1:1920x1080@60` to the single line in `/boot/firmware/cmdline.txt`, or set `force-modesetting=true` on `kmssink` (both untested). |
| Stale frame stays on the monitor after stopping a test | `sudo chvt 2; sudo chvt 1` restores the console. |
| Kernel `WARNING` in `vc4_hvs.c` (`__vc4_hvs_stop_channel`) on an HDMI hotplug event | Seen once at boot. The trace shows the framebuffer console reacting to a hotplug. No visible effect was reported. Test the monitor power cycle. |

## Design notes and findings

### Why ffmpeg and FIFOs instead of a pure GStreamer pipeline

The Pi 4 decodes HEVC with a stateless V4L2 block (`rpi-hevc-dec`) that outputs Broadcom's column-tiled SAND128 format. GStreamer 1.26.2's `v4l2slh265dec` decodes correctly, but putting its output on the display gave a garbled picture (repeating vertical strips). The decoder advertises `NV12` with the plain `BROADCOM_SAND128` modifier. The probable cause is that the column geometry never reaches the display plane, but that was never confirmed, and a newer GStreamer (1.28.x) was not tried. It also could not feed `compositor`.

Stock ffmpeg decodes the same streams correctly with `-hwaccel drm` and hands over linear frames, so the wall uses ffmpeg for decoding and GStreamer only to compose and display linear I420.

### Measurements

Per camera, CPU time over about 10 s of a 720p H.265 stream, decode only:

| Decode | CPU per camera |
|---|---|
| Hardware (`-hwaccel drm`) | about 20% of one core |
| Software | about 48% of one core |

Scaling dominates the per-camera cost. For one camera over 10 s (measured on a Pi that was under-voltage, so pessimistic):

| ffmpeg filter | CPU time |
|---|---|
| `fps=15` only | 1.5 s |
| `scale=960:540` (default bicubic) | 5.4 s |
| `scale=960:540:flags=fast_bilinear` | 3.4 s |

The whole wall at 15 fps, with a healthy supply, heatsinks and the lid off: about 1.8 of 4 cores busy (56% idle), CPU clock steady at 1500 MHz, `get_throttled` 0x0, 58 C minutes after boot and about 66 C once it had been running for a while (single readings).

The measurements above were all taken at 15 fps. A later reading with all four feeds on Enhanced was the same: 55% idle, clock 1500 MHz, `get_throttled` 0x0, 65.7 C, load average 3.2 (single reading).

At 30 fps the wall overloads the Pi 4. One reading: 8% idle (66% user, 25% system), load average 11.4 and rising, clock 1500 MHz, `get_throttled` 0x0, 70.6 C, socket `Recv-Q` about 0, each ffmpeg writing 30.7-31.0 fps, and visibly laggy output. So the cause is not power, heat or the network receive path but CPU capacity. The busiest threads were the four GStreamer `filesrc` readers (about 22% of a core each, against about 15% for each hardware decoder thread), then the compositor and its pool threads, the ffmpeg pipe writers and the Wi-Fi driver. At this rate the plumbing around the decode costs more than the decode itself. Keep `FPS` at 15 unless you measure otherwise on your own setup. Untested ways to make 30 fps viable include Ethernet instead of Wi-Fi and cutting the `filesrc` read overhead. A larger `blocksize` alone probably gains little, because each read from a FIFO returns at most the pipe buffer (64 KB by default) against a frame of about 780 KB, so the pipe size would have to be raised as well. NV12 and an intermediate rate of 20 fps were tested below, but those tests did not establish a viable 30 fps configuration. The per-frame scaling figures in the table above were measured on an under-voltage Pi, so they overstate the real cost at full clock.

With two cameras on Standard (H.264) and two on Enhanced (H.265), the H.264 feeds are decoded in software by ffmpeg (`av:h264` frame threads appear in `top`, and no ffmpeg process holds the Pi 4's H.264 block at `/dev/video10`), while the H.265 feeds still use the HEVC block. The same 15 fps wall then ran at 48% idle, clock 1500 MHz, `get_throttled` 0x0 and 68 C after about an hour. That is roughly 8 percentage points of total CPU more than the all-hardware case, about 0.3 of a core. Back on four H.265 cameras the temperature read 66 C, so the two software decodes account for only about 2 C (one reading each, so treat it as rough).

An earlier run on the same Pi with a weak supply and no cooling was throttled to 600 MHz, saturated the CPU and fell minutes behind. The hardware fixes mattered more than any pipeline tuning.

Pipeline variants at 20 fps, all four feeds on Enhanced, one 5-second `top` sample each after a 60 s settle (so differences of a point or two are noise), picture smooth in all three:

| Variant | CPU idle | `mix:src` | `filesrc` (each) |
|---|---|---|---|
| I420, with `videoconvert` | 43.5% | 11.9% | 9.9-11.3% |
| I420, without `videoconvert` | 43.6% | 12.2% | 9.6-11.6% |
| NV12 end to end, without `videoconvert` | 44.5% | 9.7% | 8.0-10.5% |

`gst-launch -v` showed `videoconvert` taking I420 and giving I420, so it was a passthrough and cost nothing. `kmssink` accepts both I420 and NV12 directly. The script therefore forces `format=I420` after the compositor (so no hidden conversion can be negotiated) and has no `videoconvert`. NV12 end to end works but gained about one point of idle CPU, within the noise, so it was not adopted.

Overall CPU idle at the three frame rates tried, all single samples: about 55% at 15 fps, about 44% at 20 fps and about 8% at 30 fps (laggy). A separate 20 fps sample taken right after a service restart read 32%, so treat single readings with caution.

### Things that did not work

| Attempt | Result |
|---|---|
| `v4l2slh265dec ! kmssink` (GStreamer 1.26.2) | Decodes, but the displayed picture is garbled (SAND128 modifier, see above). One early run also failed with "Driver did not accept the bitstream parameters" and a crash; it did not reproduce. |
| mpv from Debian Trixie (0.40) | No `v4l2request` hwdec, falls back to software. |
| Kodi | Hardware decode works, but it shows one video at a time. |
| ffmpeg `-tls_verify 0` | This ffmpeg build rejects the option for RTSP input. The connection works without it, which means the self-signed gateway certificate is not verified. Intentionally left unverified, since camwall is meant for an internal, protected network. |

### Ideas not built

- One DRM plane per camera without the compositor, which would also move scaling into the display hardware. It needs the SAND modifier problem solved and probably a small custom program using libdrm.
- A watchdog that restarts the service when a stream goes silent while the process stays alive, and black fallback tiles for dead cameras.
- Building a GStreamer wall using `v4l2h264dec` and cameras set to Standard (H.264). Switching to Standard was tested, but the current ffmpeg wall decodes those streams in software. A GStreamer wall using the Pi 4's separate H.264 block was not tried.
- Using ffmpeg's `h264_v4l2m2m` decoder for cameras on Standard, which might engage the Pi 4's H.264 block. Not tried.

## Security notes

- The tokens grant live access to the cameras. Store them only in `/etc/camwall/camwall.conf` (root:root, 0600), never in the script, the unit, the shell profile or this repository. The `.gitignore` excludes `*.env`. At runtime, systemd passes them through the service environment and the script includes each token in an ffmpeg command-line argument. Visibility to other users depends on the system's process-access permissions; the env file's permissions alone do not protect these runtime copies.
- If a token has been pasted anywhere (chat, tickets, shell history), regenerate it in Protect and update the env file.
- The service runs as an unprivileged account that is only in the `video`, `render` and `i2c` groups.
- The unit is sandboxed: no new privileges and an empty capability set, a read-only filesystem except its own state and runtime directories (`ProtectSystem=strict`, `ProtectHome`, `PrivateTmp`), no access to kernel tunables, modules, logs, control groups or the clock, and network sockets limited to IPv4, IPv6, Unix and netlink. `systemd-analyze security camwall` reports the result; the exposure level of the unit file alone went from 9.0 to 3.4 in an offline check. Device access (DRM, V4L2, DMA heaps, i2c) is not restricted yet because the exact set of nodes the decoder path needs has not been mapped on the Pi. `ProtectProc=invisible` stops the service from seeing other users' processes. It does not hide the service's own processes from other users.
- To keep the tokens out of other users' `ps` output you would have to mount `/proc` with `hidepid=2` on the whole system, which affects every login session and is not done by `install.sh`. This was not tried on the reference Pi.
- If a hardening setting breaks the wall (for example DRM permission errors right after an update), find it with `sudo systemctl edit camwall`: override one directive at a time, for example `ProtectSystem=full` or `RestrictAddressFamilies=`, restart, and report which one it was.
- The RTSPS connection does not verify the gateway certificate (see above). This is intentional: camwall is meant for an internal, protected network, so do not use it across an untrusted one.
