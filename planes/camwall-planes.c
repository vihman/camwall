/*
 * camwall-planes: 2x2 UniFi Protect wall for Raspberry Pi 4, decode-to-plane.
 *
 * Per camera: one thread, libavformat RTSPS -> V4L2 stateless HEVC hardware
 * decoder -> DRM_PRIME frames. The main thread imports each frame as a DRM
 * framebuffer and puts it on its own overlay plane; the HVS scales and
 * composes at scanout. No pixel data is touched by the CPU.
 *
 * Environment: UNIFI_HOST, TOKEN1..TOKEN4 (as in /etc/camwall/camwall.conf)
 * Optional:    CAMS (1..4, default 4), WALL_FPS (display tick, default 30)
 *
 * Build: gcc -O2 -Wall -o camwall-planes camwall-planes.c \
 *          $(pkg-config --cflags --libs libdrm libavformat libavcodec libavutil) -lpthread
 */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>

#include <xf86drm.h>
#include <xf86drmMode.h>
#include <drm_fourcc.h>
#include <drm.h>

#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libavutil/hwcontext.h>
#include <libavutil/hwcontext_drm.h>

#define MAXCAM   4
#define STALE_MS 3000   /* hide a tile when no new frame for this long */

static volatile sig_atomic_t g_stop;
static char g_card_path[64];

static int64_t now_ms(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return t.tv_sec * 1000LL + t.tv_nsec / 1000000;
}

static void say(const char *who, const char *fmt, ...)
{
    va_list ap;
    fprintf(stderr, "%s: ", who);
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fputc('\n', stderr);
}

/* ------------------------------------------------------------------ */
/* Camera side                                                         */
/* ------------------------------------------------------------------ */

typedef struct { AVFrame *frame; uint32_t fb; } Buf;

typedef struct {
    int idx;
    char name[8];
    char url[600];                 /* contains the token: never print */
    pthread_t th;
    pthread_mutex_t mu;
    AVFrame *pending;              /* newest undisplayed frame (under mu) */
    int clear;                     /* stream down (under mu) */
    int64_t last_pub;              /* ms of last published frame (under mu) */
    int64_t deadline;              /* I/O deadline for interrupt callback */
    /* display thread only */
    Buf cur, nxt;
    int nxt_set;
    uint32_t plane;
    uint32_t p_fb, p_crtc, p_sx, p_sy, p_sw, p_sh, p_cx, p_cy, p_cw, p_ch;
} Cam;

static Cam g_cam[MAXCAM];
static int g_ncam = 4;

static int intr_cb(void *opaque)
{
    Cam *c = opaque;
    return g_stop || now_ms() > c->deadline;
}

static enum AVPixelFormat get_fmt(AVCodecContext *ctx, const enum AVPixelFormat *f)
{
    (void)ctx;
    for (; *f != AV_PIX_FMT_NONE; f++)
        if (*f == AV_PIX_FMT_DRM_PRIME)
            return *f;
    return AV_PIX_FMT_NONE;
}

static void publish(Cam *c, AVFrame *f)
{
    pthread_mutex_lock(&c->mu);
    av_frame_free(&c->pending);
    c->pending = f;
    c->clear = 0;
    c->last_pub = now_ms();
    pthread_mutex_unlock(&c->mu);
}

static void set_clear(Cam *c)
{
    pthread_mutex_lock(&c->mu);
    av_frame_free(&c->pending);
    c->clear = 1;
    pthread_mutex_unlock(&c->mu);
}

static void run_stream(Cam *c)
{
    AVFormatContext *fc = avformat_alloc_context();
    AVCodecContext *ctx = NULL;
    AVBufferRef *hw = NULL;
    AVPacket *pkt = NULL;
    AVDictionary *o = NULL;
    int r, vi, fail = 0;

    if (!fc)
        return;
    fc->interrupt_callback.callback = intr_cb;
    fc->interrupt_callback.opaque = c;
    av_dict_set(&o, "rtsp_transport", "tcp", 0);
    av_dict_set(&o, "timeout", "10000000", 0);   /* us */

    c->deadline = now_ms() + 15000;
    r = avformat_open_input(&fc, c->url, NULL, &o);
    av_dict_free(&o);
    if (r < 0) {
        say(c->name, "open failed: %s", av_err2str(r));
        return;                                  /* fc freed by libavformat */
    }
    c->deadline = now_ms() + 15000;
    if (avformat_find_stream_info(fc, NULL) < 0) { say(c->name, "no stream info"); goto out; }
    vi = av_find_best_stream(fc, AVMEDIA_TYPE_VIDEO, -1, -1, NULL, 0);
    if (vi < 0) { say(c->name, "no video stream"); goto out; }
    for (unsigned i = 0; i < fc->nb_streams; i++)
        if ((int)i != vi)
            fc->streams[i]->discard = AVDISCARD_ALL;

    if (fc->streams[vi]->codecpar->codec_id != AV_CODEC_ID_HEVC) {
        say(c->name, "codec is not HEVC (set camera Encoding to Enhanced)");
        goto out;
    }
    const AVCodec *dec = avcodec_find_decoder(AV_CODEC_ID_HEVC);
    ctx = dec ? avcodec_alloc_context3(dec) : NULL;
    if (!ctx) goto out;
    avcodec_parameters_to_context(ctx, fc->streams[vi]->codecpar);

    r = av_hwdevice_ctx_create(&hw, AV_HWDEVICE_TYPE_DRM, g_card_path, NULL, 0);
    if (r < 0) { say(c->name, "hwdevice: %s", av_err2str(r)); goto out; }
    ctx->hw_device_ctx = av_buffer_ref(hw);
    ctx->get_format = get_fmt;
    ctx->extra_hw_frames = 4;       /* we hold pending + shown + queued frames */
    ctx->flags |= AV_CODEC_FLAG_LOW_DELAY;
    r = avcodec_open2(ctx, dec, NULL);
    if (r < 0) { say(c->name, "decoder open: %s", av_err2str(r)); goto out; }

    pkt = av_packet_alloc();
    int64_t last_frame = now_ms();
    int errs = 0;
    say(c->name, "streaming");
    while (!g_stop && !fail) {
        c->deadline = now_ms() + 10000;
        if (av_read_frame(fc, pkt) < 0)
            break;
        if (pkt->stream_index == vi) {
            r = avcodec_send_packet(ctx, pkt);
            if (r < 0 && r != AVERROR(EAGAIN)) {
                if (++errs > 50) { say(c->name, "decode errors, reconnecting"); fail = 1; }
            } else {
                errs = 0;
            }
            for (;;) {
                AVFrame *f = av_frame_alloc();
                r = avcodec_receive_frame(ctx, f);
                if (r < 0) {
                    av_frame_free(&f);
                    if (r != AVERROR(EAGAIN)) fail = 1;
                    break;
                }
                publish(c, f);
                last_frame = now_ms();
            }
        }
        av_packet_unref(pkt);
        if (now_ms() - last_frame > 5000) { say(c->name, "no frames for 5 s, reconnecting"); fail = 1; }
    }
out:
    set_clear(c);
    av_packet_free(&pkt);
    avcodec_free_context(&ctx);
    av_buffer_unref(&hw);
    avformat_close_input(&fc);
}

static void *cam_thread(void *arg)
{
    Cam *c = arg;
    int backoff = 1000;
    while (!g_stop) {
        int64_t t0 = now_ms();
        run_stream(c);
        if (now_ms() - t0 > 30000)
            backoff = 1000;                       /* it ran for a while: reset */
        for (int waited = 0; waited < backoff && !g_stop; waited += 100)
            usleep(100000);
        if (backoff < 5000)
            backoff += 1000;
    }
    return NULL;
}

/* ------------------------------------------------------------------ */
/* Display side                                                        */
/* ------------------------------------------------------------------ */

typedef struct {
    int fd;
    uint32_t conn, crtc, crtc_idx, primary;
    uint32_t p_conn_crtc, p_crtc_mode, p_crtc_active;
    uint32_t pr_fb, pr_crtc, pr_sx, pr_sy, pr_sw, pr_sh, pr_cx, pr_cy, pr_cw, pr_ch;
    drmModeModeInfo mode;
    uint32_t mode_blob;
    int use_primary_fb;            /* fallback when primary cannot be left off */
    uint32_t black_fb;
    int modeset_done, need_modeset, flip_pending;
    int tw, th;
} Disp;

static Disp D;

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

static int open_card(void)
{
    for (int i = 0; i < 8; i++) {
        snprintf(g_card_path, sizeof g_card_path, "/dev/dri/card%d", i);
        int fd = open(g_card_path, O_RDWR | O_CLOEXEC);
        if (fd < 0) continue;
        drmVersionPtr v = drmGetVersion(fd);
        int ok = v && !strcmp(v->name, "vc4");
        if (v) drmFreeVersion(v);
        if (ok) return fd;
        close(fd);
    }
    return -1;
}

/* pick connected connector, its CRTC and mode */
static int resolve_output(void)
{
    drmModeRes *res = drmModeGetResources(D.fd);
    if (!res) return -1;
    drmModeConnector *conn = NULL;
    for (int i = 0; i < res->count_connectors; i++) {
        drmModeConnector *c = drmModeGetConnector(D.fd, res->connectors[i]);
        if (c && c->connection == DRM_MODE_CONNECTED && c->count_modes > 0) { conn = c; break; }
        if (c) drmModeFreeConnector(c);
    }
    if (!conn) { drmModeFreeResources(res); return -1; }

    D.conn = conn->connector_id;
    D.mode = conn->modes[0];
    for (int i = 0; i < conn->count_modes; i++)
        if (conn->modes[i].type & DRM_MODE_TYPE_PREFERRED) { D.mode = conn->modes[i]; break; }

    uint32_t crtc = 0;
    if (conn->encoder_id) {
        drmModeEncoder *e = drmModeGetEncoder(D.fd, conn->encoder_id);
        if (e) { crtc = e->crtc_id; drmModeFreeEncoder(e); }
    }
    for (int i = 0; i < conn->count_encoders && !crtc; i++) {
        drmModeEncoder *e = drmModeGetEncoder(D.fd, conn->encoders[i]);
        if (!e) continue;
        for (int k = 0; k < res->count_crtcs && !crtc; k++)
            if (e->possible_crtcs & (1u << k)) crtc = res->crtcs[k];
        drmModeFreeEncoder(e);
    }
    D.crtc = crtc;
    D.crtc_idx = 0;
    for (int k = 0; k < res->count_crtcs; k++)
        if (res->crtcs[k] == crtc) D.crtc_idx = k;
    drmModeFreeConnector(conn);
    drmModeFreeResources(res);
    if (!crtc) return -1;

    D.p_conn_crtc = find_prop(D.fd, D.conn, DRM_MODE_OBJECT_CONNECTOR, "CRTC_ID", NULL);
    D.p_crtc_mode = find_prop(D.fd, D.crtc, DRM_MODE_OBJECT_CRTC, "MODE_ID", NULL);
    D.p_crtc_active = find_prop(D.fd, D.crtc, DRM_MODE_OBJECT_CRTC, "ACTIVE", NULL);

    if (D.mode_blob) drmModeDestroyPropertyBlob(D.fd, D.mode_blob);
    D.mode_blob = 0;
    if (drmModeCreatePropertyBlob(D.fd, &D.mode, sizeof D.mode, &D.mode_blob))
        return -1;
    D.tw = D.mode.hdisplay / 2;
    D.th = D.mode.vdisplay / 2;
    return 0;
}

static void load_plane_props(uint32_t plane, uint32_t *fb, uint32_t *crtc, uint32_t *sx,
                             uint32_t *sy, uint32_t *sw, uint32_t *sh, uint32_t *cx,
                             uint32_t *cy, uint32_t *cw, uint32_t *ch)
{
    int t = DRM_MODE_OBJECT_PLANE;
    *fb = find_prop(D.fd, plane, t, "FB_ID", NULL);
    *crtc = find_prop(D.fd, plane, t, "CRTC_ID", NULL);
    *sx = find_prop(D.fd, plane, t, "SRC_X", NULL);
    *sy = find_prop(D.fd, plane, t, "SRC_Y", NULL);
    *sw = find_prop(D.fd, plane, t, "SRC_W", NULL);
    *sh = find_prop(D.fd, plane, t, "SRC_H", NULL);
    *cx = find_prop(D.fd, plane, t, "CRTC_X", NULL);
    *cy = find_prop(D.fd, plane, t, "CRTC_Y", NULL);
    *cw = find_prop(D.fd, plane, t, "CRTC_W", NULL);
    *ch = find_prop(D.fd, plane, t, "CRTC_H", NULL);
}

/* assign the first g_ncam overlay planes of our CRTC, remember the primary */
static int pick_planes(void)
{
    drmModePlaneRes *pr = drmModeGetPlaneResources(D.fd);
    int n = 0;
    if (!pr) return -1;
    for (uint32_t i = 0; i < pr->count_planes; i++) {
        drmModePlane *p = drmModeGetPlane(D.fd, pr->planes[i]);
        if (!p) continue;
        if (p->possible_crtcs & (1u << D.crtc_idx)) {
            uint64_t type = 0;
            find_prop(D.fd, p->plane_id, DRM_MODE_OBJECT_PLANE, "type", &type);
            if (type == DRM_PLANE_TYPE_PRIMARY)
                D.primary = p->plane_id;
            else if (type == DRM_PLANE_TYPE_OVERLAY && n < g_ncam)
                g_cam[n++].plane = p->plane_id;
        }
        drmModeFreePlane(p);
    }
    drmModeFreePlaneResources(pr);
    if (n < g_ncam || !D.primary) return -1;
    for (int i = 0; i < g_ncam; i++) {
        Cam *c = &g_cam[i];
        load_plane_props(c->plane, &c->p_fb, &c->p_crtc, &c->p_sx, &c->p_sy, &c->p_sw,
                         &c->p_sh, &c->p_cx, &c->p_cy, &c->p_cw, &c->p_ch);
    }
    load_plane_props(D.primary, &D.pr_fb, &D.pr_crtc, &D.pr_sx, &D.pr_sy, &D.pr_sw,
                     &D.pr_sh, &D.pr_cx, &D.pr_cy, &D.pr_cw, &D.pr_ch);
    return 0;
}

static int make_black_fb(void)
{
    struct drm_mode_create_dumb cr = { .width = D.mode.hdisplay, .height = D.mode.vdisplay, .bpp = 32 };
    if (drmIoctl(D.fd, DRM_IOCTL_MODE_CREATE_DUMB, &cr)) return -1;
    struct drm_mode_map_dumb mp = { .handle = cr.handle };
    if (!drmIoctl(D.fd, DRM_IOCTL_MODE_MAP_DUMB, &mp)) {
        void *m = mmap(0, cr.size, PROT_READ | PROT_WRITE, MAP_SHARED, D.fd, mp.offset);
        if (m != MAP_FAILED) { memset(m, 0, cr.size); munmap(m, cr.size); }
    }
    uint32_t h[4] = { cr.handle }, p[4] = { cr.pitch }, o[4] = { 0 };
    return drmModeAddFB2(D.fd, D.mode.hdisplay, D.mode.vdisplay, DRM_FORMAT_XRGB8888, h, p, o,
                         &D.black_fb, 0);
}

/* import an AVDRMFrameDescriptor frame as a DRM framebuffer */
static int import_frame(const AVFrame *f, uint32_t *fb_out)
{
    const AVDRMFrameDescriptor *d = (const AVDRMFrameDescriptor *)f->data[0];
    if (!d || d->nb_layers != 1) return -1;
    const AVDRMLayerDescriptor *l = &d->layers[0];
    uint32_t oh[AV_DRM_MAX_PLANES] = { 0 };
    uint32_t hnd[4] = { 0 }, pitch[4] = { 0 }, off[4] = { 0 };
    uint64_t mod[4] = { 0 };
    int rc = -1;

    if (d->nb_objects > AV_DRM_MAX_PLANES || l->nb_planes > 4) return -1;
    for (int i = 0; i < d->nb_objects; i++)
        if (drmPrimeFDToHandle(D.fd, d->objects[i].fd, &oh[i])) goto close;
    for (int j = 0; j < l->nb_planes; j++) {
        int oi = l->planes[j].object_index;
        hnd[j] = oh[oi];
        pitch[j] = l->planes[j].pitch;
        off[j] = l->planes[j].offset;
        mod[j] = d->objects[oi].format_modifier;
    }
    rc = drmModeAddFB2WithModifiers(D.fd, f->width, f->height, l->format, hnd, pitch, off, mod,
                                    fb_out, DRM_MODE_FB_MODIFIERS);
close:
    /* the framebuffer holds its own reference to the buffer objects */
    for (int i = 0; i < d->nb_objects; i++)
        if (oh[i]) {
            struct drm_gem_close gc = { .handle = oh[i] };
            drmIoctl(D.fd, DRM_IOCTL_GEM_CLOSE, &gc);
        }
    return rc;
}

static void buf_release(Buf *b)
{
    if (b->fb) drmModeRmFB(D.fd, b->fb);
    av_frame_free(&b->frame);
    b->fb = 0;
}

static int commit(int modeset)
{
    drmModeAtomicReq *req = drmModeAtomicAlloc();
    int rc;
    if (!req) return -ENOMEM;
#define ADD(obj, prop, val) drmModeAtomicAddProperty(req, (obj), (prop), (val))
    ADD(D.crtc, D.p_crtc_active, 1);            /* keeps the CRTC in every commit (event) */
    if (modeset) {
        ADD(D.conn, D.p_conn_crtc, D.crtc);
        ADD(D.crtc, D.p_crtc_mode, D.mode_blob);
        if (D.use_primary_fb) {
            ADD(D.primary, D.pr_fb, D.black_fb);
            ADD(D.primary, D.pr_crtc, D.crtc);
            ADD(D.primary, D.pr_sx, 0);
            ADD(D.primary, D.pr_sy, 0);
            ADD(D.primary, D.pr_sw, (uint64_t)D.mode.hdisplay << 16);
            ADD(D.primary, D.pr_sh, (uint64_t)D.mode.vdisplay << 16);
            ADD(D.primary, D.pr_cx, 0);
            ADD(D.primary, D.pr_cy, 0);
            ADD(D.primary, D.pr_cw, D.mode.hdisplay);
            ADD(D.primary, D.pr_ch, D.mode.vdisplay);
        } else {
            ADD(D.primary, D.pr_fb, 0);         /* take the console off the screen */
            ADD(D.primary, D.pr_crtc, 0);
        }
    }
    for (int i = 0; i < g_ncam; i++) {
        Cam *c = &g_cam[i];
        const Buf *b = c->nxt_set ? &c->nxt : &c->cur;
        if (!b->fb) {
            ADD(c->plane, c->p_fb, 0);
            ADD(c->plane, c->p_crtc, 0);
            continue;
        }
        ADD(c->plane, c->p_fb, b->fb);
        ADD(c->plane, c->p_crtc, D.crtc);
        ADD(c->plane, c->p_sx, 0);
        ADD(c->plane, c->p_sy, 0);
        ADD(c->plane, c->p_sw, (uint64_t)b->frame->width << 16);
        ADD(c->plane, c->p_sh, (uint64_t)b->frame->height << 16);
        ADD(c->plane, c->p_cx, (i % 2) * D.tw);
        ADD(c->plane, c->p_cy, (i / 2) * D.th);
        ADD(c->plane, c->p_cw, D.tw);
        ADD(c->plane, c->p_ch, D.th);
    }
#undef ADD
    uint32_t flags = modeset ? DRM_MODE_ATOMIC_ALLOW_MODESET
                             : (DRM_MODE_ATOMIC_NONBLOCK | DRM_MODE_PAGE_FLIP_EVENT);
    rc = drmModeAtomicCommit(D.fd, req, flags, NULL);
    drmModeAtomicFree(req);
    return rc ? -errno : 0;
}

/* the queued buffers become the displayed ones; the old ones are released */
static void finalize(void)
{
    for (int i = 0; i < g_ncam; i++) {
        Cam *c = &g_cam[i];
        if (!c->nxt_set) continue;
        buf_release(&c->cur);
        c->cur = c->nxt;
        memset(&c->nxt, 0, sizeof c->nxt);
        c->nxt_set = 0;
    }
}

static void discard_queued(void)
{
    for (int i = 0; i < g_ncam; i++) {
        Cam *c = &g_cam[i];
        if (!c->nxt_set) continue;
        buf_release(&c->nxt);
        c->nxt_set = 0;
    }
}

static void flip_handler(int fd, unsigned seq, unsigned sec, unsigned usec, unsigned crtc, void *data)
{
    (void)fd; (void)seq; (void)sec; (void)usec; (void)crtc; (void)data;
    D.flip_pending = 0;
    finalize();
}

static void wait_flip(int timeout_ms)
{
    drmEventContext ev = { .version = 3, .page_flip_handler2 = flip_handler };
    struct pollfd p = { .fd = D.fd, .events = POLLIN };
    if (poll(&p, 1, timeout_ms) > 0)
        drmHandleEvent(D.fd, &ev);
}

static void on_signal(int s) { (void)s; g_stop = 1; }

int main(void)
{
    const char *host = getenv("UNIFI_HOST");
    const char *e;
    int fps = 30, stall = 0, errlog = 0;

    if ((e = getenv("CAMS")) && atoi(e) >= 1 && atoi(e) <= MAXCAM) g_ncam = atoi(e);
    if ((e = getenv("WALL_FPS")) && atoi(e) >= 1 && atoi(e) <= 60) fps = atoi(e);
    if (!host || !*host) { fprintf(stderr, "UNIFI_HOST not set\n"); return 2; }
    for (int i = 0; i < g_ncam; i++) {
        char var[16];
        snprintf(var, sizeof var, "TOKEN%d", i + 1);
        const char *tok = getenv(var);
        if (!tok || !*tok) { fprintf(stderr, "%s not set\n", var); return 2; }
        Cam *c = &g_cam[i];
        c->idx = i;
        snprintf(c->name, sizeof c->name, "cam%d", i + 1);
        snprintf(c->url, sizeof c->url, "rtsps://%s:7441/%s", host, tok);
        pthread_mutex_init(&c->mu, NULL);
        c->clear = 1;
    }

    struct sigaction sa = { .sa_handler = on_signal };
    sigaction(SIGINT, &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);
    av_log_set_level(AV_LOG_ERROR);              /* higher levels print the URL */

    D.fd = open_card();
    if (D.fd < 0) { fprintf(stderr, "no vc4 DRM device\n"); return 1; }
    drmSetClientCap(D.fd, DRM_CLIENT_CAP_UNIVERSAL_PLANES, 1);
    if (drmSetClientCap(D.fd, DRM_CLIENT_CAP_ATOMIC, 1)) { fprintf(stderr, "no atomic KMS\n"); return 1; }
    drmSetMaster(D.fd);
    if (!drmIsMaster(D.fd)) { fprintf(stderr, "not DRM master (run as root, stop camwall first)\n"); return 1; }
    if (resolve_output() || pick_planes()) { fprintf(stderr, "no usable output/planes\n"); return 1; }
    fprintf(stderr, "display %dx%d@%d, crtc %u, %d planes, tile %dx%d\n", D.mode.hdisplay,
            D.mode.vdisplay, D.mode.vrefresh, D.crtc, g_ncam, D.tw, D.th);

    for (int i = 0; i < g_ncam; i++)
        pthread_create(&g_cam[i].th, NULL, cam_thread, &g_cam[i]);

    int64_t period = 1000000000LL / fps;
    struct timespec next;
    clock_gettime(CLOCK_MONOTONIC, &next);

    while (!g_stop) {
        next.tv_nsec += period;
        while (next.tv_nsec >= 1000000000L) { next.tv_nsec -= 1000000000L; next.tv_sec++; }
        clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &next, NULL);

        if (D.flip_pending) {
            wait_flip(50);
            if (D.flip_pending) {
                if (++stall > 20) {              /* ~1 s without a flip event: redo the modeset */
                    fprintf(stderr, "flip timeout, re-running modeset\n");
                    D.flip_pending = 0; D.need_modeset = 1; stall = 0;
                    discard_queued();
                }
                continue;
            }
        }
        stall = 0;

        int changed = 0;
        int64_t t = now_ms();
        for (int i = 0; i < g_ncam; i++) {
            Cam *c = &g_cam[i];
            pthread_mutex_lock(&c->mu);
            AVFrame *nf = c->pending;
            c->pending = NULL;
            int stale = c->clear || (t - c->last_pub > STALE_MS);
            pthread_mutex_unlock(&c->mu);

            if (nf) {
                uint32_t fb = 0;
                if (import_frame(nf, &fb) == 0) {
                    c->nxt.frame = nf; c->nxt.fb = fb; c->nxt_set = 1; changed = 1;
                } else {
                    if (errlog++ < 5) say(c->name, "framebuffer import failed (errno %d)", errno);
                    av_frame_free(&nf);
                }
            } else if (stale && c->cur.fb) {
                memset(&c->nxt, 0, sizeof c->nxt);   /* hide the tile */
                c->nxt_set = 1; changed = 1;
            }
        }
        if (!changed && !D.need_modeset) continue;
        if (!D.modeset_done && !changed) continue;   /* leave the console until a frame exists */

        int modeset = !D.modeset_done || D.need_modeset;
        if (modeset && D.need_modeset && resolve_output() != 0)
            fprintf(stderr, "no connected output, will retry\n");
        int rc = commit(modeset);
        if (rc == 0) {
            if (modeset) {
                D.modeset_done = 1; D.need_modeset = 0; D.flip_pending = 0;
                finalize();
            } else {
                D.flip_pending = 1;
            }
        } else {
            if (modeset && !D.use_primary_fb && !D.modeset_done) {
                fprintf(stderr, "modeset without primary plane failed (%s), retrying with black primary\n", strerror(-rc));
                if (make_black_fb() == 0) D.use_primary_fb = 1;
            } else if (errlog++ < 10) {
                fprintf(stderr, "atomic commit failed: %s\n", strerror(-rc));
            }
            discard_queued();
            D.need_modeset = 1;
        }
    }

    for (int i = 0; i < g_ncam; i++)
        pthread_join(g_cam[i].th, NULL);
    for (int i = 0; i < g_ncam; i++) {
        buf_release(&g_cam[i].cur);
        buf_release(&g_cam[i].nxt);
    }
    close(D.fd);                                 /* the kernel restores the console */
    return 0;
}
