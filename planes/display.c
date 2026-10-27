/* Display side of camwall-planes; see display.h. */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

#include <xf86drm.h>
#include <drm_fourcc.h>
#include <drm.h>
#include <libavutil/hwcontext_drm.h>

#include "display.h"

static uint32_t find_prop(int fd, uint32_t obj, uint32_t type, const char *name, uint64_t *val)
{
    drmModeObjectProperties *p = drmModeObjectGetProperties(fd, obj, type);
    uint32_t id = 0;
    if (!p) return 0;
    for (uint32_t i = 0; i < p->count_props && !id; i++) {
        drmModePropertyRes *pr = drmModeGetProperty(fd, p->props[i]);
        if (pr) {
            if (!strcmp(pr->name, name)) {
                id = pr->prop_id;
                if (val) *val = p->prop_values[i];
            }
            drmModeFreeProperty(pr);
        }
    }
    drmModeFreeObjectProperties(p);
    return id;
}

static void load_plane_props(int fd, uint32_t plane, PlaneProps *p)
{
    int t = DRM_MODE_OBJECT_PLANE;
    p->fb = find_prop(fd, plane, t, "FB_ID", NULL);
    p->crtc = find_prop(fd, plane, t, "CRTC_ID", NULL);
    p->src_x = find_prop(fd, plane, t, "SRC_X", NULL);
    p->src_y = find_prop(fd, plane, t, "SRC_Y", NULL);
    p->src_w = find_prop(fd, plane, t, "SRC_W", NULL);
    p->src_h = find_prop(fd, plane, t, "SRC_H", NULL);
    p->crtc_x = find_prop(fd, plane, t, "CRTC_X", NULL);
    p->crtc_y = find_prop(fd, plane, t, "CRTC_Y", NULL);
    p->crtc_w = find_prop(fd, plane, t, "CRTC_W", NULL);
    p->crtc_h = find_prop(fd, plane, t, "CRTC_H", NULL);
}

static int open_card(Display *d)
{
    for (int i = 0; i < 8; i++) {
        snprintf(d->card_path, sizeof d->card_path, "/dev/dri/card%d", i);
        int fd = open(d->card_path, O_RDWR | O_CLOEXEC);
        if (fd < 0) continue;
        drmVersionPtr v = drmGetVersion(fd);
        int ok = v && !strcmp(v->name, "vc4");
        if (v) drmFreeVersion(v);
        if (ok) return fd;
        close(fd);
    }
    return -1;
}

/* pick the connected connector, its CRTC and preferred mode */
static int resolve_output(Display *d)
{
    drmModeRes *res = drmModeGetResources(d->fd);
    if (!res) return -1;
    drmModeConnector *conn = NULL;
    for (int i = 0; i < res->count_connectors; i++) {
        drmModeConnector *c = drmModeGetConnector(d->fd, res->connectors[i]);
        if (c && c->connection == DRM_MODE_CONNECTED && c->count_modes > 0) { conn = c; break; }
        if (c) drmModeFreeConnector(c);
    }
    if (!conn) { drmModeFreeResources(res); return -1; }

    d->conn = conn->connector_id;
    d->mode = conn->modes[0];
    for (int i = 0; i < conn->count_modes; i++)
        if (conn->modes[i].type & DRM_MODE_TYPE_PREFERRED) { d->mode = conn->modes[i]; break; }

    uint32_t crtc = 0;
    if (conn->encoder_id) {
        drmModeEncoder *e = drmModeGetEncoder(d->fd, conn->encoder_id);
        if (e) { crtc = e->crtc_id; drmModeFreeEncoder(e); }
    }
    for (int i = 0; i < conn->count_encoders && !crtc; i++) {
        drmModeEncoder *e = drmModeGetEncoder(d->fd, conn->encoders[i]);
        if (!e) continue;
        for (int k = 0; k < res->count_crtcs && !crtc; k++)
            if (e->possible_crtcs & (1u << k)) crtc = res->crtcs[k];
        drmModeFreeEncoder(e);
    }
    d->crtc = crtc;
    d->crtc_idx = 0;
    for (int k = 0; k < res->count_crtcs; k++)
        if (res->crtcs[k] == crtc) d->crtc_idx = k;
    drmModeFreeConnector(conn);
    drmModeFreeResources(res);
    if (!crtc) return -1;

    d->p_conn_crtc = find_prop(d->fd, d->conn, DRM_MODE_OBJECT_CONNECTOR, "CRTC_ID", NULL);
    d->p_crtc_mode = find_prop(d->fd, d->crtc, DRM_MODE_OBJECT_CRTC, "MODE_ID", NULL);
    d->p_crtc_active = find_prop(d->fd, d->crtc, DRM_MODE_OBJECT_CRTC, "ACTIVE", NULL);

    if (d->mode_blob) drmModeDestroyPropertyBlob(d->fd, d->mode_blob);
    d->mode_blob = 0;
    if (drmModeCreatePropertyBlob(d->fd, &d->mode, sizeof d->mode, &d->mode_blob))
        return -1;
    d->tw = d->mode.hdisplay / 2;
    d->th = d->mode.vdisplay / 2;
    return 0;
}

/* assign the first ntiles overlay planes of our CRTC, remember the primary */
static int pick_planes(Display *d)
{
    drmModePlaneRes *pr = drmModeGetPlaneResources(d->fd);
    int n = 0;
    if (!pr) return -1;
    for (uint32_t i = 0; i < pr->count_planes; i++) {
        drmModePlane *p = drmModeGetPlane(d->fd, pr->planes[i]);
        if (!p) continue;
        if (p->possible_crtcs & (1u << d->crtc_idx)) {
            uint64_t type = 0;
            find_prop(d->fd, p->plane_id, DRM_MODE_OBJECT_PLANE, "type", &type);
            if (type == DRM_PLANE_TYPE_PRIMARY)
                d->primary = p->plane_id;
            else if (type == DRM_PLANE_TYPE_OVERLAY && n < d->ntiles)
                d->tile[n++].plane = p->plane_id;
        }
        drmModeFreePlane(p);
    }
    drmModeFreePlaneResources(pr);
    if (n < d->ntiles || !d->primary) return -1;
    for (int i = 0; i < d->ntiles; i++)
        load_plane_props(d->fd, d->tile[i].plane, &d->tile[i].props);
    load_plane_props(d->fd, d->primary, &d->primary_props);
    return 0;
}

static int make_black_fb(Display *d)
{
    struct drm_mode_create_dumb cr = { .width = d->mode.hdisplay, .height = d->mode.vdisplay, .bpp = 32 };
    if (drmIoctl(d->fd, DRM_IOCTL_MODE_CREATE_DUMB, &cr)) return -1;
    struct drm_mode_map_dumb mp = { .handle = cr.handle };
    if (!drmIoctl(d->fd, DRM_IOCTL_MODE_MAP_DUMB, &mp)) {
        void *m = mmap(0, cr.size, PROT_READ | PROT_WRITE, MAP_SHARED, d->fd, mp.offset);
        if (m != MAP_FAILED) { memset(m, 0, cr.size); munmap(m, cr.size); }
    }
    uint32_t h[4] = { cr.handle }, p[4] = { cr.pitch }, o[4] = { 0 };
    return drmModeAddFB2(d->fd, d->mode.hdisplay, d->mode.vdisplay, DRM_FORMAT_XRGB8888, h, p, o,
                         &d->black_fb, 0);
}

/* import an AVDRMFrameDescriptor frame as a DRM framebuffer */
static int import_frame(Display *d, const AVFrame *f, uint32_t *fb_out)
{
    const AVDRMFrameDescriptor *desc = (const AVDRMFrameDescriptor *)f->data[0];
    if (!desc || desc->nb_layers != 1) return -1;
    const AVDRMLayerDescriptor *l = &desc->layers[0];
    uint32_t oh[AV_DRM_MAX_PLANES] = { 0 };
    uint32_t hnd[4] = { 0 }, pitch[4] = { 0 }, off[4] = { 0 };
    uint64_t mod[4] = { 0 };
    int rc = -1;

    if (desc->nb_objects > AV_DRM_MAX_PLANES || l->nb_planes > 4) return -1;
    for (int i = 0; i < desc->nb_objects; i++)
        if (drmPrimeFDToHandle(d->fd, desc->objects[i].fd, &oh[i])) goto close;
    for (int j = 0; j < l->nb_planes; j++) {
        int oi = l->planes[j].object_index;
        hnd[j] = oh[oi];
        pitch[j] = l->planes[j].pitch;
        off[j] = l->planes[j].offset;
        mod[j] = desc->objects[oi].format_modifier;
    }
    rc = drmModeAddFB2WithModifiers(d->fd, f->width, f->height, l->format, hnd, pitch, off, mod,
                                    fb_out, DRM_MODE_FB_MODIFIERS);
close:
    /* the framebuffer holds its own reference to the buffer objects */
    for (int i = 0; i < desc->nb_objects; i++)
        if (oh[i]) {
            struct drm_gem_close gc = { .handle = oh[i] };
            drmIoctl(d->fd, DRM_IOCTL_GEM_CLOSE, &gc);
        }
    return rc;
}

static void buf_release(Display *d, Buf *b)
{
    if (b->fb) drmModeRmFB(d->fd, b->fb);
    av_frame_free(&b->frame);
    b->fb = 0;
}

/* show fb on a plane: source w x h (whole buffer) into the given screen rectangle */
static void add_plane(drmModeAtomicReq *req, uint32_t plane, const PlaneProps *p, uint32_t crtc,
                      uint32_t fb, uint32_t w, uint32_t h, int x, int y, int cw, int ch)
{
    drmModeAtomicAddProperty(req, plane, p->fb, fb);
    drmModeAtomicAddProperty(req, plane, p->crtc, crtc);
    drmModeAtomicAddProperty(req, plane, p->src_x, 0);
    drmModeAtomicAddProperty(req, plane, p->src_y, 0);
    drmModeAtomicAddProperty(req, plane, p->src_w, (uint64_t)w << 16);   /* 16.16 fixed point */
    drmModeAtomicAddProperty(req, plane, p->src_h, (uint64_t)h << 16);
    drmModeAtomicAddProperty(req, plane, p->crtc_x, x);
    drmModeAtomicAddProperty(req, plane, p->crtc_y, y);
    drmModeAtomicAddProperty(req, plane, p->crtc_w, cw);
    drmModeAtomicAddProperty(req, plane, p->crtc_h, ch);
}

static void add_plane_off(drmModeAtomicReq *req, uint32_t plane, const PlaneProps *p)
{
    drmModeAtomicAddProperty(req, plane, p->fb, 0);
    drmModeAtomicAddProperty(req, plane, p->crtc, 0);
}

static int commit(Display *d, int modeset)
{
    drmModeAtomicReq *req = drmModeAtomicAlloc();
    int rc;
    if (!req) return -ENOMEM;
    drmModeAtomicAddProperty(req, d->crtc, d->p_crtc_active, 1);   /* keeps the CRTC in every commit (event) */
    if (modeset) {
        drmModeAtomicAddProperty(req, d->conn, d->p_conn_crtc, d->crtc);
        drmModeAtomicAddProperty(req, d->crtc, d->p_crtc_mode, d->mode_blob);
        if (d->use_primary_fb)
            add_plane(req, d->primary, &d->primary_props, d->crtc, d->black_fb,
                      d->mode.hdisplay, d->mode.vdisplay, 0, 0, d->mode.hdisplay, d->mode.vdisplay);
        else
            add_plane_off(req, d->primary, &d->primary_props);   /* take the console off the screen */
    }
    for (int i = 0; i < d->ntiles; i++) {
        Tile *t = &d->tile[i];
        const Buf *b = t->nxt_set ? &t->nxt : &t->cur;
        if (!b->fb || (d->solo >= 0 && i != d->solo))
            add_plane_off(req, t->plane, &t->props);
        else if (d->solo >= 0)                            /* one camera, full screen */
            add_plane(req, t->plane, &t->props, d->crtc, b->fb, b->frame->width, b->frame->height,
                      0, 0, d->mode.hdisplay, d->mode.vdisplay);
        else
            add_plane(req, t->plane, &t->props, d->crtc, b->fb, b->frame->width, b->frame->height,
                      (i % 2) * d->tw, (i / 2) * d->th, d->tw, d->th);
    }
    uint32_t flags = modeset ? DRM_MODE_ATOMIC_ALLOW_MODESET
                             : (DRM_MODE_ATOMIC_NONBLOCK | DRM_MODE_PAGE_FLIP_EVENT);
    rc = drmModeAtomicCommit(d->fd, req, flags, d);
    drmModeAtomicFree(req);
    return rc ? -errno : 0;
}

/* the queued buffers become the displayed ones; the old ones are released */
static void finalize(Display *d)
{
    for (int i = 0; i < d->ntiles; i++) {
        Tile *t = &d->tile[i];
        if (!t->nxt_set) continue;
        buf_release(d, &t->cur);
        t->cur = t->nxt;
        memset(&t->nxt, 0, sizeof t->nxt);
        t->nxt_set = 0;
    }
}

static void discard_queued(Display *d)
{
    for (int i = 0; i < d->ntiles; i++) {
        Tile *t = &d->tile[i];
        if (!t->nxt_set) continue;
        buf_release(d, &t->nxt);
        t->nxt_set = 0;
    }
}

static void flip_handler(int fd, unsigned seq, unsigned sec, unsigned usec, unsigned crtc, void *data)
{
    Display *d = data;
    (void)fd; (void)seq; (void)sec; (void)usec; (void)crtc;
    d->flip_pending = 0;
    finalize(d);
}

static void wait_flip(Display *d, int timeout_ms)
{
    drmEventContext ev = { .version = 3, .page_flip_handler2 = flip_handler };
    struct pollfd p = { .fd = d->fd, .events = POLLIN };
    if (poll(&p, 1, timeout_ms) > 0)
        drmHandleEvent(d->fd, &ev);
}

int display_open(Display *d, int ntiles)
{
    d->ntiles = ntiles;
    d->solo = -1;
    d->fd = open_card(d);
    if (d->fd < 0) { fprintf(stderr, "no vc4 DRM device\n"); return -1; }
    drmSetClientCap(d->fd, DRM_CLIENT_CAP_UNIVERSAL_PLANES, 1);
    if (drmSetClientCap(d->fd, DRM_CLIENT_CAP_ATOMIC, 1)) { fprintf(stderr, "no atomic KMS\n"); return -1; }
    drmSetMaster(d->fd);
    if (!drmIsMaster(d->fd)) { fprintf(stderr, "not DRM master (run as root, stop camwall first)\n"); return -1; }
    if (resolve_output(d) || pick_planes(d)) { fprintf(stderr, "no usable output/planes\n"); return -1; }
    fprintf(stderr, "display %dx%d@%d, crtc %u, %d planes, tile %dx%d\n", d->mode.hdisplay,
            d->mode.vdisplay, d->mode.vrefresh, d->crtc, d->ntiles, d->tw, d->th);
    return 0;
}

int display_ready(Display *d)
{
    if (d->flip_pending) {
        wait_flip(d, 50);
        if (d->flip_pending) {
            if (++d->stall > 20) {               /* ~1 s without a flip event: redo the modeset */
                fprintf(stderr, "flip timeout, re-running modeset\n");
                d->flip_pending = 0; d->need_modeset = 1; d->stall = 0;
                discard_queued(d);
            }
            return -1;
        }
    }
    d->stall = 0;
    return 0;
}

int display_update_tile(Display *d, int i, AVFrame *f, int stale, const char *who)
{
    Tile *t = &d->tile[i];
    if (f) {
        uint32_t fb = 0;
        if (import_frame(d, f, &fb) == 0) {
            t->nxt.frame = f; t->nxt.fb = fb; t->nxt_set = 1;
            return 1;
        }
        if (d->errlog++ < 5) say(who, "framebuffer import failed (errno %d)", errno);
        av_frame_free(&f);
    } else if (stale && t->cur.fb) {
        memset(&t->nxt, 0, sizeof t->nxt);   /* hide the tile */
        t->nxt_set = 1;
        return 1;
    }
    return 0;
}

void display_present(Display *d, int changed)
{
    if (!changed && !d->need_modeset) return;
    if (!d->modeset_done && !changed) return;    /* leave the console until a frame exists */

    int modeset = !d->modeset_done || d->need_modeset;
    if (modeset && d->need_modeset && resolve_output(d) != 0)
        fprintf(stderr, "no connected output, will retry\n");
    int rc = commit(d, modeset);
    if (rc == 0) {
        if (modeset) {
            d->modeset_done = 1; d->need_modeset = 0; d->flip_pending = 0;
            finalize(d);
        } else {
            d->flip_pending = 1;
        }
    } else {
        if (modeset && !d->use_primary_fb && !d->modeset_done) {
            fprintf(stderr, "modeset without primary plane failed (%s), retrying with black primary\n", strerror(-rc));
            if (make_black_fb(d) == 0) d->use_primary_fb = 1;
        } else if (d->errlog++ < 10) {
            fprintf(stderr, "atomic commit failed: %s\n", strerror(-rc));
        }
        discard_queued(d);
        d->need_modeset = 1;
    }
}

void display_close(Display *d)
{
    for (int i = 0; i < d->ntiles; i++) {
        buf_release(d, &d->tile[i].cur);
        buf_release(d, &d->tile[i].nxt);
    }
    close(d->fd);                                /* the kernel restores the console */
}
