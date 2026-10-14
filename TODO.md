# TODO

Findings from a read-through of the repo. Nothing here has been run or measured yet;
performance items need before/after numbers on the Pi before they are adopted.

## Cleanup

- [ ] Commit the pending edits (`README.md`, `bin/camwall`, `config/tokens.env.example`).
- [ ] Derive the compositor size from `W` and `H`. `bin/camwall` hardcodes `1920x1080` separately from `W=960; H=540`, and the README tells people to edit both by hand. Use `$((2*W))x$((2*H))` and read `W`/`H` from the environment.
- [ ] Tidy `.gitignore`: `tokens.env` is already covered by `*.env`.
- [ ] Add lint checks: `shellcheck bin/camwall`, `systemd-analyze verify systemd/camwall.service`, a markdown lint. Wire them into pre-commit and a CI workflow.
- [x] Add an install script (`install.sh`: packages, user and groups, `i2c-dev`, config from the example, script and unit, restart only on change). Dry run checked on a non-Pi machine only; run it on the Pi.
- [ ] Split the README: move Measurements, Troubleshooting and Design notes into `docs/`; keep install and operation in the README.
- [ ] Move the README snippets into scripts: `tools/check-hwdec.sh` (fuser, interrupt counter, A/B benchmark) and `tools/set-fps.sh`.
- [ ] Add a bash version guard. `wait -n "${pids[@]}"` needs bash 5.1+; fail early with a clear message.
- [ ] Turn the README's "Ideas not built" list into issues (or fold it into this file). Add a LICENSE if the repo will be shared.

## Documentation fixes

- [ ] Move `FPS` into the env file (commented-out `#FPS=15` in the example, default stays 15 in the script). Then drop the README's drop-in instructions, the "don't define `FPS` in the env file" pitfall and the `systemctl edit` pitfall, and make the `bin/camwall` header consistent. Keep the README's `ps | grep fps=` check; it still applies.
- [ ] Decide on renaming `tokens.env` to `camwall.conf` (it also holds `UNIFI_HOST` and would hold `FPS`, so "tokens" is stale). If yes, in one change: rename `config/tokens.env.example` to `config/camwall.conf.example`; update `EnvironmentFile=` in the unit, the error messages in `bin/camwall`, and every README path; and add `camwall.conf` to `.gitignore` (the current `*.env` rule would no longer cover the real file, so a secret could be committed). Existing installs need a migration note: move `/etc/camwall/tokens.env` to `camwall.conf` (keep mode 0600 root:root) before restarting with the new unit, otherwise the service fails to start.
- [ ] Explain `Environment=HOME=/var/lib/camwall` and `StateDirectory=camwall` in the README (probably GStreamer's registry cache; confirm), since the README says the service user has no home directory.
- [ ] Update README step 3 (change `W`, `H` and the compositor size by hand) once the compositor size is derived from `W`/`H`.
- [ ] Reword the README's "larger `blocksize`" 30 fps suggestion to match the pipe-size finding (see the `filesrc` item under Performance).
- [ ] State the network (Wi-Fi) in the Tested configuration table; the measurements depend on it.
- [ ] List `TODO.md` in the README's Repository layout block.
- [ ] Mention `sudo systemctl daemon-reload` in the "Update the script" row when the unit file changed too.
- [ ] Move the lab-notebook text (earlier runs, under-voltage figures, one-off failures) into `docs/measurements.md` with the README split.

## Power and display (monitor on a zigbee plug)

Findings (2026-10-14, Pi 4, vc4-kms, monitor on HDMI-A-1):
- HDMI connector status cannot detect the monitor power being cut: `status` stays `connected`, and a forced probe (`modetest -M vc4 -c`) still reports `connected` with the EDID (530x300 mm), even when the Pi booted with the monitor unpowered. The EDID is evidently served from the Pi's 5V. Boot config has no hotplug overrides, so that is not the cause.
- DDC/CI works as a presence probe: `ddcutil getvcp d6` prints `Display not found` when the monitor is unpowered and `DPM: On` when it is on. It needs the `i2c-dev` module (`sudo modprobe i2c-dev`).
- Timing run (one probe every ~2 s plus call time): failures start within ~3 s of the cut; 6 consecutive failures over ~22 s while off; success again once the monitor was up; no failures in 12 calls with the monitor on (small sample).

Implemented in `bin/camwall` (probe loop with `DISPLAY_PROBE`, `PROBE_INTERVAL`, `PROBE_FAILS`, `PROBE_SETTLE`), the `i2c` group in the unit and the README. Tested only against stub `ffmpeg` / `gst-launch-1.0` / `ddcutil` binaries; not yet run on the Pi.

- [ ] Test on the Pi: install the new script and unit, cut and restore the plug, check the journal transitions, that ffmpeg/gst processes disappear while idle, and that the picture comes back.
- [ ] Verify the `camwall` user can run `ddcutil getvcp d6` (group `i2c`, `i2c-dev` loaded at boot via `/etc/modules-load.d/i2c.conf`).
- [ ] Check that killing the pipeline on display loss leaves no stuck ffmpeg (a writer blocked on a FIFO) and that `kmssink` releases the display cleanly.
- [ ] Journal noise from the probe is silenced with `--syslog never` (verified by hand on the Pi, not yet via the service). Next: log how long each failed probe takes while idle; a boot with the monitor off took about 30 s from the last ddcutil message to `display detected` and it is unclear whether that is the monitor's boot time or a slow probe. Optionally add a `--bus` setting (the probe checks buses 20 and 21, one per HDMI port).
- [ ] Measure the probe's own cost: plain `ddcutil getvcp d6` does display detection on every call; use `--bus=N` if it shows up in `top`.
- [ ] Soak test: probe for several hours with the monitor on and count false failures; add retries or `--sleep-multiplier` if there are any.
- [ ] Fallback if DDC proves flaky: have the plug's controller (Home Assistant / zigbee2mqtt) stop and start `camwall` (SSH with a restricted sudoers rule, or a `mosquitto_sub` service on the Pi).
- [ ] Measure power with the wall stopped vs running, and record what the 17 W reading includes (Pi only, or Pi plus monitor).

## Hardening and robustness

- [x] `ProtectProc=invisible` added. Correction to the original idea: it hides *other* processes from the service, not the tokens from other users. Hiding the ffmpeg command lines from other users needs `hidepid=2` on `/proc` host-wide (documented in the README, not automated). `ProcSubset=pid` was left out because it hides `/proc/stat`, `/proc/meminfo` and `/proc/sys`, which libraries may read; try it on the Pi if wanted.
- [x] Harden the unit: `NoNewPrivileges`, empty `CapabilityBoundingSet`, `ProtectSystem=strict`, `ProtectHome`, `PrivateTmp`, kernel/cgroup/clock/hostname protections, `RestrictAddressFamilies` (with `AF_NETLINK` for the resolver), `RestrictNamespaces`, `UMask=0077`. Offline exposure score 9.0 -> 3.4. Not yet run on the Pi: start the service and check that the wall still comes up, the decoder is held (`fuser -v /dev/video19`), the display probe works and the journal shows no permission errors.
- [ ] Restrict device access (`DevicePolicy=closed` plus `DeviceAllow=`): needs the full node list first (DRM card and render nodes, `/dev/video*`, `/dev/media*`, `/dev/dma_heap/*`, `/dev/i2c-*`). Find it with the wall running (`fuser -v /dev/dri/* /dev/video* /dev/media* /dev/dma_heap/* /dev/i2c-*`) before enabling anything.
- [ ] Consider `SystemCallFilter=@system-service`; skipped because `MemoryDenyWriteExecute` and syscall filters can break ffmpeg and GStreamer (ORC JIT) and could not be tested here.
- [x] FIFOs now live in `RuntimeDirectory=camwall` (`$RUNTIME_DIRECTORY/fifos`), with a `mktemp -d` fallback for running by hand. The script still removes its FIFO directory when it stops the wall, because the service stays up while idle.
- [x] ffmpeg gets `-timeout` (from `STREAM_TIMEOUT`, default 10 s). Not yet run on the Pi. `-fflags nobuffer -flags low_delay` and `-map 0:v:0` were tried and removed again after the tile widths looked uneven; the cause was never confirmed, so retry them separately, one flag at a time, only on a test Pi.
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
