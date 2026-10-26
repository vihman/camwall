# Measurements

Lab notebook of the reference Pi (see the [README](../README.md#tested-configuration) for the hardware). Unless stated otherwise these are single readings over Wi-Fi (the Pi moved to Ethernet on 2026-10-26, see [Ethernet](#ethernet)), so treat differences of a few points as noise.

## Decode

Per camera, CPU time over about 10 s of a 720p H.265 stream, decode only:

| Decode | CPU per camera |
|---|---|
| Hardware (`-hwaccel drm`) | about 20% of one core |
| Software | about 48% of one core |

## Scaling

Scaling dominates the per-camera cost. For one camera over 10 s (measured on a Pi that was under-voltage, so pessimistic):

| ffmpeg filter | CPU time |
|---|---|
| `fps=15` only | 1.5 s |
| `scale=960:540` (default bicubic) | 5.4 s |
| `scale=960:540:flags=fast_bilinear` | 3.4 s |

## Whole wall by frame rate

At 15 fps, with a healthy supply, heatsinks and the lid off: about 1.8 of 4 cores busy (56% idle), CPU clock steady at 1500 MHz, `get_throttled` 0x0, 58 C minutes after boot and about 66 C once it had been running for a while. A later reading with all four feeds on Enhanced was the same: 55% idle, clock 1500 MHz, `get_throttled` 0x0, 65.7 C, load average 3.2.

At 24 fps: 23.6% idle, load average 5.8.

At 30 fps the wall overloads the Pi 4. One reading: 8% idle (66% user, 25% system), load average 11.4 and rising, clock 1500 MHz, `get_throttled` 0x0, 70.6 C, socket `Recv-Q` about 0, each ffmpeg writing 30.7-31.0 fps, and visibly laggy output. So the cause is not power, heat or the network receive path but CPU capacity. The busiest threads were the four GStreamer `filesrc` readers (about 22% of a core each, against about 15% for each hardware decoder thread), then the compositor and its pool threads, the ffmpeg pipe writers and the Wi-Fi driver. At this rate the plumbing around the decode costs more than the decode itself. Keep `FPS` at 15 unless you measure otherwise on your own setup.

Untested ways to make 30 fps viable with this design include Ethernet instead of Wi-Fi and cutting the `filesrc` read overhead. A larger `blocksize` alone probably gains little, because each read from a FIFO returns at most the pipe buffer (64 KB by default) against a frame of about 780 KB, so the pipe size would have to be raised as well. The decode-to-plane design (see [Design](design.md#decode-to-plane-experimental)) removes this plumbing altogether.

Overall CPU idle at the frame rates tried: about 55% at 15 fps, about 44% at 20 fps, 23.6% at 24 fps and about 8% at 30 fps (laggy). A separate 20 fps sample taken right after a service restart read 32%.

## Ethernet

2026-10-26, GStreamer wall at 15 fps, Wi-Fi radio off, four minutes after a restart. Three 5-second `top` samples: 58.5%, 55.1% and 53.5% idle (about 56% on average), load average 3.0, clock 1500 MHz, `get_throttled` 0x0, 69.1 C at 18 minutes after boot. CMA: 512 MB total, about 401 MB free, so the four decoders hold about 110 MB.

At 30 fps on Ethernet the wall still saturates the CPU. Three 5-second samples starting right after a restart read 26.4%, 1.1% and 12.6% idle (the first one partly during startup). Load average 5.85 and rising, 15 runnable threads, clock 1500 MHz, `get_throttled` 0x0, 70.6 C. Per thread it looks the same as on Wi-Fi: each `filesrc` reader uses about 21% of a core, the compositor (`mix:src` plus pools) about 53% in total, each hardware decode thread 13-15% and each ffmpeg rawvideo writer 7-8%. The network was never what limited 30 fps; the pixel path is.

At 24 fps on Ethernet, about 10 minutes after a restart: 30.5%, 28.0% and 26.8% idle, load average 6.93 / 6.58 / 5.52 (levelling off at about 7), 73.5 C and steady, `get_throttled` 0x0. That is a few points better than the single Wi-Fi reading at 24 fps (23.6%). CPU and temperature are sustainable. Even so, a load of about 7 on 4 cores, against 3 at 15 fps, means threads regularly wait to run. The wall still keeps up: after about 20 minutes, all four camera sockets had 0 bytes in their receive queues (see [Checking that the wall keeps up](operation.md#checking-that-the-wall-keeps-up)). 24 fps is the highest rate that works on Ethernet.

The four Medium (720p H.265) streams arrived at 73-104 kB/s each, about 0.6-0.8 Mbit/s.

The 15 fps result matches the 15 fps Wi-Fi reading (about 55%), so at 15 fps the network was not a factor; the Wi-Fi driver only showed among the busiest threads at 30 fps. The busiest threads are still the pixel path rather than the decode: per camera, the hardware decode thread (`dec0:2:hevc`) uses 9-14% of a core. On top of that, each GStreamer `filesrc` reader uses 6-10%, and the compositor (`mix:src` plus three `pool-N` threads) uses about 30% in total.

## Pipeline variants at 20 fps

All four feeds on Enhanced, one 5-second `top` sample each after a 60 s settle (so differences of a point or two are noise), picture smooth in all three:

| Variant | CPU idle | `mix:src` | `filesrc` (each) |
|---|---|---|---|
| I420, with `videoconvert` | 43.5% | 11.9% | 9.9-11.3% |
| I420, without `videoconvert` | 43.6% | 12.2% | 9.6-11.6% |
| NV12 end to end, without `videoconvert` | 44.5% | 9.7% | 8.0-10.5% |

`gst-launch -v` showed `videoconvert` taking I420 and giving I420, so it was a passthrough and cost nothing. `kmssink` accepts both I420 and NV12 directly. The script therefore forces `format=I420` after the compositor (so no hidden conversion can be negotiated) and has no `videoconvert`. NV12 end to end works but gained about one point of idle CPU, within the noise, so it was not adopted.

## Mixed H.264 and H.265

With two cameras on Standard (H.264) and two on Enhanced (H.265), the H.264 feeds are decoded in software by ffmpeg (`av:h264` frame threads appear in `top`, and no ffmpeg process holds the Pi 4's H.264 block at `/dev/video10`), while the H.265 feeds still use the HEVC block. The same 15 fps wall then ran at 48% idle, clock 1500 MHz, `get_throttled` 0x0 and 68 C after about an hour. That is roughly 8 percentage points of total CPU more than the all-hardware case, about 0.3 of a core. Back on four H.265 cameras the temperature read 66 C, so the two software decodes account for only about 2 C.

## Power and cooling

An earlier run on the same Pi with a weak supply and no cooling was throttled to 600 MHz, saturated the CPU and fell minutes behind. The hardware fixes mattered more than any pipeline tuning.

Power (smart-plug meter), one reading each, taken at different moments: 17 W with the Pi (wall running) and the monitor on the plug, 13 W with the monitor alone. That puts the Pi at about 4 W at that moment only. It is not a typical or maximum figure: the Pi's draw varies with load and was never measured over time or with the wall stopped. The monitor is the larger part, and the plug cut already covers it.

## Decode-to-plane (experimental)

One stream through `ffmpeg -hwaccel drm -hwaccel_output_format drm_prime -f vout_drm -`: 97.1% idle (one sample, night/IR scene).

`camwall-planes` with all four cameras, 2026-10-26, Ethernet, run as root (`systemd-run`), about 4 minutes after start: three 5-second samples at 93.8%, 94.2% and 93.3% idle, load average 0.40 (falling from the GStreamer wall's history), 58.9 C and falling, `get_throttled` 0x0. CMA: 512 MB total, about 439 MB free, so the decoders hold about 75 MB. The process has 70 MB resident and the system 167 threads, against 275 with the GStreamer wall. The busiest threads are the display loop (about 5% of a core) and the four camera threads (about 3% each). Picture clean, all four camera clocks advancing in step.

Compared with the GStreamer wall at 24 fps on Ethernet (about 28% idle, load about 7, 73.5 C), the plane wall uses about a twelfth of the CPU, and it puts the newest frame of each camera's full stream on screen, rather than a reduced rate. The decoder's interrupt counter rose by 1206 in 5 seconds (about 241 per second). That is the same as with the GStreamer wall, because both decode every frame. At about two interrupts per frame (the decoder works in two stages), that is about 30 fps per camera. The display updates on its own 30 Hz tick, so a frame can occasionally be skipped.

Overnight run, 2026-10-26, as user `camwall`: 7 hours without a restart or any journal line after a deliberate camera restart at the start. After 7 hours, 73.2 MB resident (about 70 MB shortly after an earlier start), CMA about 443 MB free (about 449 MB in the earlier run), 55.0 C, load average 0.4. No sign of a leak. The start values come from a different run of the program, so a trend from one continuous run is still to be confirmed.

So lowering `FPS` on the GStreamer wall never reduced decoding. It only reduced the copying, scaling and composing after the decoder, which is where that wall's CPU goes.
