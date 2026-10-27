/*
 * Camera side: one thread per camera, libavformat RTSPS -> V4L2 stateless HEVC
 * hardware decoder -> DRM_PRIME frames. Knows nothing about the display; the
 * main loop takes the newest frame with stream_take().
 */
#ifndef CAMWALL_STREAM_H
#define CAMWALL_STREAM_H

#include <pthread.h>
#include <stdint.h>

#include <libavutil/frame.h>

typedef struct {
    char name[16];                 /* "cam1".."cam4", for log lines */
    char url[600];                 /* contains the token: never print */
    const char *drm_path;          /* DRM device for the hardware decoder */
    pthread_t th;
    pthread_mutex_t mu;
    AVFrame *pending;              /* newest frame not yet taken (under mu) */
    int down;                      /* stream is down (under mu) */
    int64_t last_pub;              /* ms of the last published frame (under mu) */
    int64_t deadline;              /* I/O deadline for the interrupt callback */
} Stream;

void stream_init(Stream *s, int idx, const char *host, const char *token);
void stream_start(Stream *s, const char *drm_path);
void stream_join(Stream *s);

/* Take the newest frame, or NULL if none arrived since the last call; the caller
 * owns it. *stale is set when the stream is down or has published nothing for
 * longer than stale_ms. */
AVFrame *stream_take(Stream *s, int64_t now, int64_t stale_ms, int *stale);

#endif
