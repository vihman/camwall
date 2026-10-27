/*
 * camwall-planes: 2x2 RTSP camera wall for Raspberry Pi 4, decode-to-plane.
 *
 * Per camera: one thread, libavformat RTSP(S) -> hardware decoder (HEVC: V4L2
 * stateless; H.264: v4l2m2m) -> DRM_PRIME frames (stream.c). The main thread
 * imports each frame as a DRM framebuffer and puts it on its own overlay plane
 * (display.c); the HVS scales and composes at scanout. No pixel data is touched
 * by the CPU.
 *
 * Environment: CAM1_URL..CAM4_URL, already expanded by camwalld
 * Optional:    CAMS (1..4, default 4), WALL_FPS (display tick, default 30)
 * Missing configuration exits with 78 (EX_CONFIG); camwalld does not restart on it.
 * Normally started by camwalld (bin/camwalld), built and installed by install.sh.
 *
 * Build: make (see Makefile)
 */
#define _GNU_SOURCE
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include <libavutil/log.h>

#include "common.h"
#include "display.h"
#include "stream.h"

#define STALE_MS 3000   /* hide a tile when no new frame for this long */

volatile sig_atomic_t g_stop;

int64_t now_ms(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return t.tv_sec * 1000LL + t.tv_nsec / 1000000;
}

void say(const char *who, const char *fmt, ...)
{
    va_list ap;
    fprintf(stderr, "%s: ", who);
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fputc('\n', stderr);
}

static void on_signal(int s) { (void)s; g_stop = 1; }

static Stream g_stream[MAXCAM];
static Display g_disp;

int main(void)
{
    const char *e;
    int ncam = MAXCAM, fps = 30;

    if ((e = getenv("CAMS")) && atoi(e) >= 1 && atoi(e) <= MAXCAM) ncam = atoi(e);
    if ((e = getenv("WALL_FPS")) && atoi(e) >= 1 && atoi(e) <= 60) fps = atoi(e);
    for (int i = 0; i < ncam; i++) {
        char var[16];
        snprintf(var, sizeof var, "CAM%d_URL", i + 1);
        const char *url = getenv(var);
        if (!url || !*url) { fprintf(stderr, "%s not set\n", var); return 78; }
        if (strchr(url, '<')) {
            fprintf(stderr, "%s contains a <VARIABLE>: start the wall through camwalld\n", var);
            return 78;
        }
        if (strlen(url) >= sizeof g_stream[i].url) { fprintf(stderr, "%s too long\n", var); return 78; }
        stream_init(&g_stream[i], i, url);
    }

    struct sigaction sa = { .sa_handler = on_signal };
    sigaction(SIGINT, &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);
    av_log_set_level(AV_LOG_ERROR);              /* higher levels print the URL */

    if (display_open(&g_disp, ncam))
        return 1;
    for (int i = 0; i < ncam; i++)
        stream_start(&g_stream[i], g_disp.card_path);

    int64_t period = 1000000000LL / fps;
    struct timespec next;
    clock_gettime(CLOCK_MONOTONIC, &next);

    while (!g_stop) {
        next.tv_nsec += period;
        while (next.tv_nsec >= 1000000000L) { next.tv_nsec -= 1000000000L; next.tv_sec++; }
        clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &next, NULL);

        if (display_ready(&g_disp))
            continue;

        /* move each camera's newest frame to its tile */
        int changed = 0;
        int64_t t = now_ms();
        for (int i = 0; i < ncam; i++) {
            int stale;
            AVFrame *f = stream_take(&g_stream[i], t, STALE_MS, &stale);
            changed |= display_update_tile(&g_disp, i, f, stale, g_stream[i].name);
        }
        display_present(&g_disp, changed);
    }

    for (int i = 0; i < ncam; i++)
        stream_join(&g_stream[i]);
    display_close(&g_disp);
    return 0;
}
