/* Camera side of camwall-planes; see stream.h. */
#define _GNU_SOURCE
#include <stdio.h>
#include <unistd.h>

#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libavutil/hwcontext.h>

#include "common.h"
#include "stream.h"

static int intr_cb(void *opaque)
{
    Stream *s = opaque;
    return g_stop || now_ms() > s->deadline;
}

static enum AVPixelFormat get_fmt(AVCodecContext *ctx, const enum AVPixelFormat *f)
{
    for (; *f != AV_PIX_FMT_NONE; f++)
        if (*f == AV_PIX_FMT_DRM_PRIME)
            return *f;
    say(((Stream *)ctx->opaque)->name, "%s offers no DRM_PRIME output", ctx->codec->name);
    return AV_PIX_FMT_NONE;
}

/* newest frame wins: an untaken older frame is dropped */
static void publish(Stream *s, AVFrame *f)
{
    pthread_mutex_lock(&s->mu);
    av_frame_free(&s->pending);
    s->pending = f;
    s->down = 0;
    s->last_pub = now_ms();
    pthread_mutex_unlock(&s->mu);
}

static void set_down(Stream *s)
{
    pthread_mutex_lock(&s->mu);
    av_frame_free(&s->pending);
    s->down = 1;
    pthread_mutex_unlock(&s->mu);
}

/* one connection: open, decode until error or stop */
static void run_stream(Stream *s)
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
    fc->interrupt_callback.opaque = s;
    av_dict_set(&o, "rtsp_transport", "tcp", 0);
    av_dict_set(&o, "timeout", "10000000", 0);   /* us */

    s->deadline = now_ms() + 15000;
    r = avformat_open_input(&fc, s->url, NULL, &o);
    av_dict_free(&o);
    if (r < 0) {
        say(s->name, "open failed: %s", av_err2str(r));
        return;                                  /* fc freed by libavformat */
    }
    s->deadline = now_ms() + 15000;
    if (avformat_find_stream_info(fc, NULL) < 0) { say(s->name, "no stream info"); goto out; }
    vi = av_find_best_stream(fc, AVMEDIA_TYPE_VIDEO, -1, -1, NULL, 0);
    if (vi < 0) { say(s->name, "no video stream"); goto out; }
    for (unsigned i = 0; i < fc->nb_streams; i++)
        if ((int)i != vi)
            fc->streams[i]->discard = AVDISCARD_ALL;

    /* HEVC: the stateless rpi-hevc-dec through the drm hwaccel. H.264: the Pi's
       separate stateful H.264 block through v4l2m2m. Both hand over DRM_PRIME frames. */
    enum AVCodecID id = fc->streams[vi]->codecpar->codec_id;
    const AVCodec *dec = id == AV_CODEC_ID_HEVC ? avcodec_find_decoder(id)
                       : id == AV_CODEC_ID_H264 ? avcodec_find_decoder_by_name("h264_v4l2m2m")
                       : NULL;
    if (!dec) {
        say(s->name, "no decoder for %s (use H.265 or H.264)", avcodec_get_name(id));
        goto out;
    }
    ctx = avcodec_alloc_context3(dec);
    if (!ctx) goto out;
    ctx->opaque = s;
    avcodec_parameters_to_context(ctx, fc->streams[vi]->codecpar);

    r = av_hwdevice_ctx_create(&hw, AV_HWDEVICE_TYPE_DRM, s->drm_path, NULL, 0);
    if (r < 0) { say(s->name, "hwdevice: %s", av_err2str(r)); goto out; }
    ctx->hw_device_ctx = av_buffer_ref(hw);
    ctx->get_format = get_fmt;
    ctx->extra_hw_frames = 4;       /* we hold pending + shown + queued frames */
    ctx->flags |= AV_CODEC_FLAG_LOW_DELAY;
    r = avcodec_open2(ctx, dec, NULL);
    if (r < 0) { say(s->name, "decoder open: %s", av_err2str(r)); goto out; }

    pkt = av_packet_alloc();
    int64_t last_frame = now_ms();
    int errs = 0;
    say(s->name, "streaming (%s)", dec->name);
    while (!g_stop && !fail) {
        s->deadline = now_ms() + 10000;
        if (av_read_frame(fc, pkt) < 0)
            break;
        if (pkt->stream_index == vi) {
            r = avcodec_send_packet(ctx, pkt);
            if (r < 0 && r != AVERROR(EAGAIN)) {
                if (++errs > 50) { say(s->name, "decode errors, reconnecting"); fail = 1; }
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
                publish(s, f);
                last_frame = now_ms();
            }
        }
        av_packet_unref(pkt);
        if (now_ms() - last_frame > 5000) { say(s->name, "no frames for 5 s, reconnecting"); fail = 1; }
    }
out:
    set_down(s);
    av_packet_free(&pkt);
    avcodec_free_context(&ctx);
    av_buffer_unref(&hw);
    avformat_close_input(&fc);
}

/* reconnect forever with a growing delay (1-5 s), until stopped */
static void *stream_thread(void *arg)
{
    Stream *s = arg;
    int backoff = 1000;
    while (!g_stop) {
        int64_t t0 = now_ms();
        run_stream(s);
        if (now_ms() - t0 > 30000)
            backoff = 1000;                       /* it ran for a while: reset */
        for (int waited = 0; waited < backoff && !g_stop; waited += 100)
            usleep(100000);
        if (backoff < 5000)
            backoff += 1000;
    }
    return NULL;
}

void stream_init(Stream *s, int idx, const char *url)
{
    snprintf(s->name, sizeof s->name, "cam%d", idx + 1);
    snprintf(s->url, sizeof s->url, "%s", url);
    pthread_mutex_init(&s->mu, NULL);
    s->down = 1;
}

void stream_start(Stream *s, const char *drm_path)
{
    s->drm_path = drm_path;
    pthread_create(&s->th, NULL, stream_thread, s);
}

void stream_join(Stream *s)
{
    pthread_join(s->th, NULL);
}

AVFrame *stream_take(Stream *s, int64_t now, int64_t stale_ms, int *stale)
{
    pthread_mutex_lock(&s->mu);
    AVFrame *f = s->pending;
    s->pending = NULL;
    *stale = s->down || (now - s->last_pub > stale_ms);
    pthread_mutex_unlock(&s->mu);
    return f;
}
