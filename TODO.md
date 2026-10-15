# TODO

Findings from a read-through of the repo. Nothing here has been run or measured yet;
performance items need before/after numbers on the Pi before they are adopted.

## Cleanup

- [x] Commit the pending edits.
- [x] Tile size is configurable (`TILE_WIDTH`, `TILE_HEIGHT`, default 960x540, even, 64-3840) and the compositor size is derived as 2x that. Replaces the hardcoded `1920x1080`.
- [x] `.gitignore` covers `*.env` (old name) and `camwall.conf`; the old `tokens.env` entry is kept on purpose so a leftover file cannot be committed.
- [ ] Add lint checks: `shellcheck bin/camwall`, `systemd-analyze verify systemd/camwall.service`, a markdown lint. Wire them into pre-commit and a CI workflow.
- [x] Add an install script (`install.sh`: packages, user and groups, `i2c-dev`, config from the example, script and unit, restart only on change). Dry run checked on a non-Pi machine only; run it on the Pi.
- [ ] Split the README: move Measurements, Troubleshooting and Design notes into `docs/`; keep install and operation in the README.
- [ ] Move the README snippets into scripts: `tools/check-hwdec.sh` (fuser, interrupt counter, A/B benchmark) and `tools/set-fps.sh`.
- [ ] Add a bash version guard. `wait -n "${pids[@]}"` needs bash 5.1+; fail early with a clear message.
- [ ] Turn the README's "Ideas not built" list into issues (or fold it into this file). Add a LICENSE if the repo will be shared.

## Documentation fixes

- [x] Move `FPS` into the env file (commented-out `#FPS=15` in the example, default stays 15 in the script). Then drop the README's drop-in instructions, the "don't define `FPS` in the env file" pitfall and the `systemctl edit` pitfall, and make the `bin/camwall` header consistent. Keep the README's `ps | grep fps=` check; it still applies.
- [x] Renamed `tokens.env` to `camwall.conf` (`config/camwall.conf.example` now lists every setting with its default). `install.sh` moves an existing `/etc/camwall/tokens.env` to the new name before installing the unit; by hand: `sudo mv /etc/camwall/tokens.env /etc/camwall/camwall.conf`. Not yet run on the Pi.
- [x] Explain `Environment=HOME=/var/lib/camwall` and `StateDirectory=camwall` in the README (probably GStreamer's registry cache; confirm), since the README says the service user has no home directory.
- [x] README step 3 now points to `TILE_WIDTH` / `TILE_HEIGHT`.
- [x] Reword the README's "larger `blocksize`" 30 fps suggestion to match the pipe-size finding (see the `filesrc` item under Performance).
- [x] State the network (Wi-Fi) in the Tested configuration table; the measurements depend on it.
- [x] List `TODO.md` in the README's Repository layout block.
- [x] "Update the script and unit" row now says `git pull && ./install.sh`, which reloads and restarts as needed.
- [ ] Move the lab-notebook text (earlier runs, under-voltage figures, one-off failures) into `docs/measurements.md` with the README split.

## Power and display (monitor on a zigbee plug)

Findings (2026-10-14, Pi 4, vc4-kms, monitor on HDMI-A-1):
- HDMI connector status cannot detect the monitor power being cut: `status` stays `connected`, and a forced probe (`modetest -M vc4 -c`) still reports `connected` with the EDID (530x300 mm), even when the Pi booted with the monitor unpowered. The EDID is evidently served from the Pi's 5V. Boot config has no hotplug overrides, so that is not the cause.
- DDC/CI works as a presence probe: `ddcutil getvcp d6` prints `Display not found` when the monitor is unpowered and `DPM: On` when it is on. It needs the `i2c-dev` module (`sudo modprobe i2c-dev`).
- Timing run (one probe every ~2 s plus call time): failures start within ~3 s of the cut; 6 consecutive failures over ~22 s while off; success again once the monitor was up; no failures in 12 calls with the monitor on (small sample).
- The monitor answers DDC for features it does not support (`Feature 0xdd should not exist but ... succeeds`); `ddcutil` copes with it and `getvcp d6` still gives a real answer, but it probably adds a second or two per probe.
- Booting the Pi with the monitor off leaves the console at the 1024x768 fallback mode, and `kmssink` then crops the picture (uneven tile widths). Reboot with the monitor on fixes it. Accepted as a corner case; the README troubleshooting table describes it.

Implemented in `bin/camwall` (probe loop with `DISPLAY_PROBE`, `PROBE_INTERVAL`, `PROBE_FAILS`, `PROBE_SETTLE`), the `i2c` group in the unit and the README. Running in production since 2026-10-15: cutting and restoring the monitor plug stops and restarts the wall, and a boot with the monitor off waits for it. What is left below is measurement and longer-term checks.

- [x] Tested on the Pi: plug cut and restore stops and restarts the wall, and the picture comes back. Boot with the monitor off also works.
- [x] The `camwall` user runs `ddcutil` under the service (group `i2c`, `i2c-dev` loaded at boot); seen in the journal of a boot with the monitor off.
- [x] After a plug cut (once the failure window has passed) no ffmpeg or `gst-launch-1.0` process is left and `/run/camwall/fifos` is gone, so the stop path works with real ffmpeg. `sudo fuser -v /dev/dri/card1` shows no holder, so `kmssink` released the display.
- [x] Journal noise from the probe is silenced with `--syslog never` (no `ddcutil` lines after the service restart on 2026-10-15).
- [ ] Optional: log how long each failed probe takes while idle. A boot with the monitor off took about 30 s from the last `ddcutil` message to `display detected`, and it is unclear whether that is the monitor's boot time or a slow probe.
- [x] Probe cost measured on the Pi while idle (monitor unpowered): 1272 ms CPU in 60 s, about 2% of one core. Accepted. If it ever matters, raise `PROBE_INTERVAL` (10 halves it) rather than adding `--bus`.
- [ ] Soak test: probe for several hours with the monitor on and count false failures; add retries or `--sleep-multiplier` if there are any.
- [ ] Fallback if DDC proves flaky: have the plug's controller (Home Assistant / zigbee2mqtt) stop and start `camwall` (SSH with a restricted sudoers rule, or a `mosquitto_sub` service on the Pi).
- [ ] Measure power with the wall stopped vs running, and record what the 17 W reading includes (Pi only, or Pi plus monitor).

## Hardening and robustness

- [x] `ProtectProc=invisible` added. Correction to the original idea: it hides *other* processes from the service, not the tokens from other users. Hiding the ffmpeg command lines from other users needs `hidepid=2` on `/proc` host-wide (documented in the README, not automated). `ProcSubset=pid` was left out because it hides `/proc/stat`, `/proc/meminfo` and `/proc/sys`, which libraries may read; try it on the Pi if wanted.
- [x] Harden the unit: `NoNewPrivileges`, empty `CapabilityBoundingSet`, `ProtectSystem=strict`, `ProtectHome`, `PrivateTmp`, kernel/cgroup/clock/hostname protections, `RestrictAddressFamilies` (with `AF_NETLINK` for the resolver), `RestrictNamespaces`, `UMask=0077`. Offline exposure score 9.0 -> 3.4. Not yet run on the Pi: start the service and check that the wall still comes up, the decoder is held (`fuser -v /dev/video19`), the display probe works and the journal shows no permission errors.
- [ ] Restrict device access (`DevicePolicy=closed` plus `DeviceAllow=`): needs the full node list first (DRM card and render nodes, `/dev/video*`, `/dev/media*`, `/dev/dma_heap/*`, `/dev/i2c-*`). Find it with the wall running (`fuser -v /dev/dri/* /dev/video* /dev/media* /dev/dma_heap/* /dev/i2c-*`) before enabling anything.
- [ ] Consider `SystemCallFilter=@system-service`; skipped because `MemoryDenyWriteExecute` and syscall filters can break ffmpeg and GStreamer (ORC JIT) and could not be tested here.
- [x] FIFOs now live in `RuntimeDirectory=camwall` (`$RUNTIME_DIRECTORY/fifos`), with a `mktemp -d` fallback for running by hand. The script still removes its FIFO directory when it stops the wall, because the service stays up while idle.
- [x] ffmpeg gets `-timeout` (from `STREAM_TIMEOUT`, default 10 s). Not yet run on the Pi. `-fflags nobuffer -flags low_delay` and `-map 0:v:0` were removed after the tile widths looked wrong, but that turned out to be the 1024x768 fallback mode from booting with the monitor off, not these flags. They were never tested on the Pi; if latency matters, retry them one at a time on a test Pi.
- [ ] Verify on the Pi that a stalled stream really makes ffmpeg exit within `STREAM_TIMEOUT` (for example block one camera's traffic with a firewall rule, not by unplugging the monitor), and that nothing else changes in the picture.
- [x] Add restart backoff (`RestartSteps=4`, `RestartMaxDelaySec=60`, plus `RestartPreventExitStatus=78` for config errors). Not yet run on the Pi.
- [ ] Verify on the Pi that the restart delay grows on repeated failures and check (docs do not say) whether it drops back to 3 s after a long healthy run.
- [x] Add a startup preflight (`preflight`, `check_hwdec` in `bin/camwall`: missing tools fatal, missing or inaccessible `rpi-hevc-dec`, no `drm` hwaccel and ffmpeg not holding the decoder node only warn). Tested with stubs only; confirm on the Pi that the real `/proc/PID/fd` check sees `/dev/video19`.
- [x] Validate `FPS` (whole number 1-100) and the `PROBE_*` settings; config errors exit 78. Missing `UNIFI_HOST` and tokens are now checked at startup, before waiting for the display, and also exit 78.

## Performance

- [ ] Reduce `filesrc` read overhead (busiest thread at 30 fps). `blocksize` alone likely gains little: a FIFO holds 64 KB by default, so each `read()` returns at most that, against a ~780 KB I420 frame. The real limit is the pipe size (`F_SETPIPE_SZ`, capped by `/proc/sys/fs/pipe-max-size`, 1 MB by default); neither ffmpeg nor `filesrc` sets it. Options to test: a small helper that raises the pipe size, or a Unix socket instead of a FIFO (ffmpeg `unix:` output, larger socket buffers). Measure first; drop this item if there is no gain.
- [ ] Re-run the 30 fps test on Ethernet with the `blocksize` change, taking several samples instead of single readings.
- [ ] Try the hardware H.264 path for cameras on Standard: `h264_v4l2m2m`, picking the decoder per camera via `ffprobe`.
- [ ] Add black fallback tiles. The compositor waits for all four inputs, so one dead camera blanks the wall. Feed a `videotestsrc` fallback and set `ignore-inactive-pads` / `latency` on the compositor, or run a per-camera reconnect loop.
