# Installation by hand

[`install.sh`](../install.sh) automates steps 2, 4, 5 (only creating the file) and 6 below (see the [README](../README.md#quick-start)). This page is the same thing done by hand, plus the manual steps 1 and 3.

Do all of this over SSH. Check the prompt names the Pi before running any command, because a dropped SSH session silently leaves you in your local shell and the commands then run on the wrong machine.

## 1. Enable RTSP streams in UniFi Protect

For each camera, open its settings in Protect, go to the RTSP section, and enable the **Medium** stream (1280x720). The UI shows a URL like `rtsps://<gateway>:7441/<TOKEN>?enableSrtp`. The token is the last path segment. Medium suits the 960x540 tiles; Low (640x360) would have to be upscaled.

Treat the token as a credential: anyone on the network who has it can watch that camera. Regenerate it in Protect if it ever leaks.

**Leave the camera's video encoding on Enhanced.** In Protect the setting is under the camera's Recording Settings, Recording Quality, Encoding, and offers Standard, Enhanced and Advanced. In UniFi's wording Enhanced is H.265 (HEVC) and Standard is H.264 (a third-party Protect review describes it that way). The setting sits in the Recording Quality block, so Protect presents it as a recording setting, but it also governs the RTSP streams. On the reference system all four cameras were on Enhanced and all four streams came out as H.265, which is what this wall is built around. Switching one camera to Standard changed its Medium RTSP stream to H.264 (1280x720, 30 fps); that was tested on one camera. What Advanced does is not known. If you switch to Standard, check the codec with `ffmpeg -hide_banner -rtsp_transport tcp -i "rtsps://<gateway>:7441/<TOKEN>"` before relying on the wall. A camera on Standard still works, but its H.264 stream is decoded in software (see [Measurements](measurements.md)), so it costs more CPU than an Enhanced camera.

## 2. Install packages

```bash
sudo apt update
sudo apt install -y ffmpeg gstreamer1.0-tools gstreamer1.0-plugins-base \
  gstreamer1.0-plugins-good gstreamer1.0-plugins-bad v4l-utils libdrm-tests ddcutil
```

`v4l-utils` and `libdrm-tests` are only for the checks in the next step.

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
sudoedit /etc/camwall/camwall.conf      # set UNIFI_HOST and TOKEN1..TOKEN4
sudo ls -l /etc/camwall/camwall.conf    # expect -rw------- root root
```

The file holds the gateway, the four tokens and all optional settings (frame rate, tile size, stream timeout, display check), each commented-out with its default in the example. systemd reads it as root before it drops to the `camwall` user, so the service account never needs read access to it. The format is plain `KEY=value` lines, and comments must be on their own lines (systemd does not accept trailing comments).

Upgrading from a version that used `/etc/camwall/tokens.env`: `install.sh` moves the file to `camwall.conf` for you. By hand, run `sudo mv /etc/camwall/tokens.env /etc/camwall/camwall.conf` before installing the new unit, otherwise the service cannot start.

## 6. Install the script and the unit, then start

```bash
sudo install -m 0755 -o root -g root bin/camwall /usr/local/bin/camwall
sudo install -m 0644 -o root -g root systemd/camwall.service /etc/systemd/system/camwall.service
sudo systemctl daemon-reload
sudo systemctl enable --now camwall
```

The wall should appear within a few seconds.

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
- **Decoder interrupts.** The interrupt counter of the HEVC decoder (IRQ `feb00000.codec`) rose by about 1200 in 5 seconds (roughly 240 per second) while the wall decoded 4 cameras at 15 fps, which is 60 frames per second in total.
- **CPU cost.** Four concurrent streams cost about 1.9-2.0 s of CPU time per camera over a ~9.5 s run with `-hwaccel drm`, against 4.3-4.9 s without it.
- **Kernel driver message.** `dmesg` showed a line from the decoder's kernel driver (`rpi-hevc-dec feb00000.codec: Missing inuse DPB ent 0, timestamp=0`) at the start of the hardware-decode runs that were checked, and not during software-decode runs.

To repeat the checks on your own Pi while the wall (or any `-hwaccel drm` ffmpeg) is running:

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
    -i "rtsps://$UNIFI_HOST:7441/$TOKEN1" -an -t 10 -f null - 2>&1 | tr '\r' '\n' | grep 'bench: utime'
done
```

The device number (`/dev/video19`) is the one `v4l2-ctl --list-devices` reports for `rpi-hevc-dec` on the reference Pi; it can differ on yours. For check 3, export `UNIFI_HOST` and `TOKEN1` into your shell first (the config file is root-only, so type them in; avoid leaving a token in your shell history). The `utime + stime` of the hardware run should be well below the software run.
