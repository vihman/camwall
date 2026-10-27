/*
 * Display side: the vc4 KMS device, the connected output and one overlay plane
 * per tile. Frames are imported as DRM framebuffers and shown with atomic
 * commits; the HVS scales and composes at scanout. Knows nothing about RTSP.
 */
#ifndef CAMWALL_DISPLAY_H
#define CAMWALL_DISPLAY_H

#include <stdint.h>

#include <xf86drmMode.h>
#include <libavutil/frame.h>

#include "common.h"

/* KMS property ids of one plane */
typedef struct {
    uint32_t fb, crtc;
    uint32_t src_x, src_y, src_w, src_h;
    uint32_t crtc_x, crtc_y, crtc_w, crtc_h;
} PlaneProps;

/* a decoded frame and the framebuffer it was imported as */
typedef struct { AVFrame *frame; uint32_t fb; } Buf;

/* one tile of the wall: an overlay plane and what it shows */
typedef struct {
    uint32_t plane;
    PlaneProps props;
    Buf cur;                       /* on screen */
    Buf nxt;                       /* queued for the next commit; empty buf = hide */
    int nxt_set;
} Tile;

typedef struct {
    int fd;
    char card_path[64];            /* also used for the decoder's hardware device */
    uint32_t conn, crtc, crtc_idx, primary;
    uint32_t p_conn_crtc, p_crtc_mode, p_crtc_active;
    PlaneProps primary_props;
    drmModeModeInfo mode;
    uint32_t mode_blob;
    int use_primary_fb;            /* fallback when the primary cannot be left off */
    uint32_t black_fb;
    int modeset_done, need_modeset, flip_pending, stall;
    int tw, th;                    /* tile size: half the mode */
    int errlog;                    /* limits repeated error lines */
    Tile tile[MAXCAM];
    int ntiles;
    int solo;                      /* -1: the 2x2 grid; else the one tile shown full screen */
} Display;

/* Open the vc4 card as DRM master and pick the output and ntiles overlay planes.
 * Starts in the grid layout (solo = -1). Prints the reason and returns -1 on failure.
 * Change d->solo at any time; the next display_present(d, 1) applies it. */
int display_open(Display *d, int ntiles);

/* 0 if a new commit may be made, -1 while a page flip is still pending
 * (after about a second without a flip event it schedules a new modeset). */
int display_ready(Display *d);

/* Queue tile i's next content: frame f (owned by the display from now on), or,
 * with f == NULL and stale set, an empty tile. Returns 1 if anything changed. */
int display_update_tile(Display *d, int i, AVFrame *f, int stale, const char *who);

/* Commit the queued changes (with a modeset first time and after errors). */
void display_present(Display *d, int changed);

void display_close(Display *d);

#endif
