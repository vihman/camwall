# Installation by hand

[`install.sh`](../install.sh) automates steps 2, 4, 5 (only creating the file) and 6 below (see the [README](../README.md#quick-start)). This page is the same thing done by hand, plus the manual steps 1 and 3.

Do all of this over SSH. Check the prompt names the Pi before running any command, because a dropped SSH session silently leaves you in your local shell and the commands then run on the wrong machine.

## 1. Find the stream URLs

camwall needs one `rtsp://` or `rtsps://` URL per tile, from the cameras themselves or from a recorder (NVR) that re-serves them. Each stream must be H.265 or H.264; other codecs (MJPEG, AV1) are refused and leave the tile black. About 1280x720 suits the 960x540 tiles: much smaller streams are upscaled, much larger ones cost decoder time for nothing. The camera's or recorder's documentation gives the URL format; check a URL with `tools/feed-info.sh -` (see [Operation](operation.md#measuring-and-checking)) before relying on it.

Treat the URLs as credentials: a token or password in them lets anyone on the network watch that camera.

Only UniFi Protect has been tested (below). Other sources should work if they serve such streams over RTSP with TCP, which camwall always uses.

### UniFi Protect

For each camera, open its settings in Protect, go to the RTSP section, and enable the **Medium** stream (1280x720). The UI shows a URL like `rtsps://<gateway>:7441/<TOKEN>?enableSrtp`. Use it without `?enableSrtp`. The token is the last path segment, so the config can share the host and port: `CAM1_URL=rtsps://<STREAM_HOST>:<STREAM_PORT>/<TOKEN1>` with `STREAM_PORT=7441`. Medium suits the 960x540 tiles; Low (640x360) would have to be upscaled. Regenerate a token in Protect if it ever leaks.

**Keep the camera on H.265: Super Encoding checked, set to Auto.** In current Protect the setting is under the camera's Recording Settings, Recording Quality, as a **Super Encoding** checkbox with Auto or AV1. Older Protect versions show it as Encoding: Standard, Enhanced or Advanced. Protect presents it as a recording setting, but it governs the RTSP streams too. Confirmed on the reference system (2026-10-27): Super Encoding unchecked sends H.264, checked with Auto sends H.265 (in the old UI, Standard sent H.264 and Enhanced H.265). H.264 gave no sharpness gain: Protect sent its Medium stream at about the same bitrate (0.31 against 0.39 Mbit/s for an H.265 camera, 2026-10-27). Do not pick AV1 (probably the old Advanced): the Pi 4 has no AV1 decoder, and with a camera on AV1 for about 10 s the Pi's ffprobe could not identify the stream's codec (2026-10-27).

### H.264 streams

The wall is built and measured around H.265. H.264 also works: the plane wall decodes it on the Pi's separate H.264 block with FFmpeg's `h264_v4l2m2m` (tested on the Pi with all four cameras: the same CPU as H.265, about 23 MB more CMA per camera). That block allocates from the GPU's own memory, not CMA: with the default `gpu_mem=76` it failed with more than one camera (`Not enough GPU mem?` in `dmesg`, black tiles), and with `gpu_mem=256` in `/boot/firmware/config.txt` (under `[all]`, then reboot) all four ran. Lower values between those were not tried. H.265 streams do not need it. The GStreamer wall decodes H.264 in software (see [Measurements](measurements.md)), which costs more CPU.

## 2. Install packages

```bash
sudo apt update
sudo apt install -y ffmpeg gstreamer1.0-tools gstreamer1.0-plugins-base \
  gstreamer1.0-plugins-good gstreamer1.0-plugins-bad v4l-utils libdrm-tests ddcutil \
  python3 gcc pkg-config libdrm-dev libavformat-dev libavcodec-dev libavutil-dev
```

The `-dev` packages, `gcc` and `pkg-config` are for building the plane wall in step 6. They must come from the Raspberry Pi OS archive, whose FFmpeg build carries the Pi's hardware-decode support. `ffmpeg` and the GStreamer packages are for the GStreamer fallback. `v4l-utils` and `libdrm-tests` are only for the checks in the next step.

## 3. Check the hardware

```bash
# HEVC hardware decoder: expect rpi-hevc-dec with a /dev/videoN node (/dev/video19 on the reference Pi)
v4l2-ctl --list-devices | grep -A2 rpi-hevc-dec

# Display: expect a connected HDMI connector and a 1920x1080 mode marked "preferred"
modetest -M vc4 -c | grep -E 'connected|preferred'
```

If the monitor's preferred mode is not 1920x1080, set `TILE_WIDTH` and `TILE_HEIGHT` in the configuration (step 5) to half of the mode's width and height, for example `640` and `360` for 1280x720. Both must be even. The wall is 2x2 tiles, so the screen size is derived from them.

## 4. Create the service account

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

## 5. Install the configuration

```bash
sudo install -d -m 0755 /etc/camwall
sudo install -m 0600 -o root -g root config/camwall.conf.example /etc/camwall/camwall.conf
sudoedit /etc/camwall/camwall.conf      # set CAM1_URL..CAM4_URL
sudo ls -l /etc/camwall/camwall.conf    # expect -rw------- root root
```

The file holds the four camera URLs and all optional settings (frame rate, tile size, stream timeout, display check), each commented-out with its default in the example. In a URL, `<NAME>` is replaced with the value of the variable `NAME` from the same file, so a shared host, port, token or password is written once; camwalld does the replacement and refuses to start (status 78) if a variable is missing. systemd reads the file as root before it drops to the `camwall` user, so the service account never needs read access to it. The format is plain `KEY=value` lines, and comments must be on their own lines (systemd does not accept trailing comments). Because of the `<NAME>` templates the file cannot be loaded with the shell's `.` or `source`; the tools ask `camwalld --url N` for an expanded URL instead.

Upgrading from a version that used `UNIFI_HOST` and `TOKEN1..4`: `install.sh` renames `UNIFI_HOST` to `STREAM_HOST` and appends `STREAM_PORT=7441` and `CAMn_URL=rtsps://<STREAM_HOST>:<STREAM_PORT>/<TOKENn>`, keeping the old file as `camwall.conf.bak` (0600). By hand, make the same edit. From a version that used `/etc/camwall/tokens.env`: `install.sh` moves the file to `camwall.conf` first; by hand, `sudo mv /etc/camwall/tokens.env /etc/camwall/camwall.conf`.

## 6. Build the plane wall, install the programs and the unit, then start

```bash
gcc -O2 -Wall -o /tmp/camwall-planes planes/*.c \
  $(pkg-config --cflags --libs libdrm libavformat libavcodec libavutil) -lpthread
sudo install -m 0755 -o root -g root /tmp/camwall-planes /usr/local/bin/camwall-planes
sudo install -m 0755 -o root -g root bin/camwalld /usr/local/bin/camwalld
sudo install -m 0755 -o root -g root bin/camwall /usr/local/bin/camwall
sudo install -m 0644 -o root -g root systemd/camwall.service /etc/systemd/system/camwall.service
sudo systemctl daemon-reload
sudo systemctl enable --now camwall
```

The wall should appear within a few seconds. `journalctl -u camwall` shows `started the planes wall` and then `camN: streaming (hevc)` for each camera (`h264_v4l2m2m` for a camera on H.264).

## 7. Verify

```bash
systemctl status camwall --no-pager
journalctl -u camwall -f              # Ctrl-C leaves the service running
vcgencmd measure_clock arm            # ~1500 MHz under load means no throttling
vcgencmd get_throttled                # 0x0 means no under-voltage or thermal throttling
vcgencmd measure_temp
```

Then reboot once (`sudo reboot`) and confirm the wall comes up without anybody logging in. Also switch the monitor off and on with the service running and check that the picture comes back.

## Confirming the hardware decoder is in use

ffmpeg's `-hwaccel` option can fall back to software decoding silently, so check it instead of assuming. On the reference Pi, with the wall running as the `camwall` service, the hardware decoder was confirmed in use:

- **Decoder device held open.** `fuser -v /dev/video19` listed all four ffmpeg processes. That is the node the script's own check looks at. ffmpeg itself reports the decoder as `/dev/media2,/dev/video19`, so the media node number differs between Pis and boots and is not needed for the check.
- **Decoder interrupts.** The interrupt counter of the HEVC decoder (IRQ `feb00000.codec`) rose by about 1200 in 5 seconds (roughly 240 per second) with four cameras. The decoder probably raises two interrupts per frame (it works in two stages), so this is about 120 frames per second in total: each camera's full 30 fps. ffmpeg decodes every frame; the wall's `fps=` filter drops frames only after the decoder, so the count does not depend on `FPS`. The plane wall gives the same count.
- **CPU cost.** Four concurrent streams cost about 1.9-2.0 s of CPU time per camera over a ~9.5 s run with `-hwaccel drm`, against 4.3-4.9 s without it.
- **Kernel driver message.** `dmesg` showed a line from the decoder's kernel driver (`rpi-hevc-dec feb00000.codec: Missing inuse DPB ent 0, timestamp=0`) at the start of the hardware-decode runs that were checked, and not during software-decode runs.

`tools/check-hwdec.sh` runs checks 1 and 2 below, and with `--ab` also check 3. To repeat the checks by hand while the wall (or any `-hwaccel drm` ffmpeg) is running:

```bash
# 1. The decoder device node should be held open by the ffmpeg processes
sudo fuser -v /dev/video19

# 2. The decoder's interrupt counter should rise between two samples.
#    The IRQ is named after the device in dmesg (feb00000.codec on the reference Pi).
grep -iE 'hevc|rpivid|feb00000' /proc/interrupts; sleep 5; grep -iE 'hevc|rpivid|feb00000' /proc/interrupts

# 3. A direct A/B on one stream: CPU seconds with and without the hardware decoder
for opt in "-hwaccel drm" ""; do
  echo "== ffmpeg $opt"
  ffmpeg -nostdin -hide_banner -loglevel info -benchmark -rtsp_transport tcp $opt \
    -i "$URL" -an -t 10 -f null - 2>&1 | tr '\r' '\n' | grep 'bench: utime'
done
```

The device number (`/dev/video19`) is the one `v4l2-ctl --list-devices` reports for `rpi-hevc-dec` on the reference Pi; it can differ on yours. For check 3, put camera 1's stream URL in `URL` first without leaving it in your shell history, for example `read -rs URL` and paste it. The `utime + stime` of the hardware run should be well below the software run.
