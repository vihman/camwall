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
 * Optional:    CAMS (1..4, default 4), WALL_FPS (display tick, default 30),
 *              WALL_LAYOUT (0 = the 2x2 grid, default; N = camera N full screen)
 * stdin:       lines "layout N" switch the layout while running (camwalld sends
 *              them); all cameras keep decoding, so a switch is immediate.
 * Missing configuration exits with 78 (EX_CONFIG); camwalld does not restart on it.
 * Normally started by camwalld (bin/camwalld), built and installed by install.sh.
 *
 * Build: make (see Makefile)
 */
#define _GNU_SOURCE
#include <fcntl.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

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

/* "0" -> -1 (grid), "1".."ncam" -> tile index, anything else -> -2 */
static int parse_layout(const char *s, int ncam)
{
    char *end;
    long n = strtol(s, &end, 10);
    if (end == s || *end || n < 0 || n > ncam) return -2;
    return n == 0 ? -1 : (int)n - 1;
}

/* Apply complete "layout N" lines from stdin (non-blocking). Returns 1 if the
 * layout changed. EOF (no controller, or stdin is /dev/null) stops the reading. */
static int read_commands(int ncam)
{
    static char buf[128];
    static size_t len;
    static int closed;
    int changed = 0;

    if (closed) return 0;
    ssize_t n = read(0, buf + len, sizeof buf - 1 - len);
    if (n == 0) { closed = 1; return 0; }
    if (n < 0) return 0;                         /* EAGAIN: nothing new */
    len += n;
    buf[len] = 0;
    char *line = buf, *nl;
    while ((nl = strchr(line, '\n'))) {
        *nl = 0;
        int solo;
        if (strncmp(line, "layout ", 7) == 0 && (solo = parse_layout(line + 7, ncam)) != -2) {
            changed |= solo != g_disp.solo;
            g_disp.solo = solo;
        } else {
            say("wall", "ignoring command '%.40s'", line);
        }
        line = nl + 1;
    }
    len -= line - buf;
    memmove(buf, line, len);
    if (len == sizeof buf - 1) len = 0;          /* an overlong line: drop it */
    return changed;
}

int main(void)
{
    const char *e;
    int ncam = MAXCAM, fps = 30;

    if ((e = getenv("CAMS")) && atoi(e) >= 1 && atoi(e) <= MAXCAM) ncam = atoi(e);
    if ((e = getenv("WALL_FPS")) && atoi(e) >= 1 && atoi(e) <= 60) fps = atoi(e);
    for (int i = 0; i < ncam; i++) {
        char var[24];
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
    if ((e = getenv("WALL_LAYOUT")) && *e) {
        int solo = parse_layout(e, ncam);
        if (solo == -2) say("wall", "WALL_LAYOUT must be 0..%d, using the grid", ncam);
        else g_disp.solo = solo;
    }
    fcntl(0, F_SETFL, fcntl(0, F_GETFL) | O_NONBLOCK);
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
        int changed = read_commands(ncam);
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
