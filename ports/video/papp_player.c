// psram_video's player: FFmpeg demuxing and decoding on the PAPP loader.
//
//   vdemux  opens the input (papp_io.c behind a custom AVIOContext), finds
//           the streams, opens the decoders, then reads packets into two
//           queues (video, audio). It performs seeks: a seek bumps `serial`
//           and sends a flush marker down each queue.
//   vvideo  decodes video, drops frames that are already late on the clock
//           (and asks the decoder to skip work while it is behind), converts
//           the rest to RGB565 into a ring of frame slots.
//   vaudio  decodes audio, converts it to 16-bit stereo (libswresample) and
//           hands it to audio_submit, which blocks while the speaker's buffer
//           is full. What it has handed over, minus the buffer, is the clock.
//   main    (papp_play, on the loader's worker task) shows each frame when
//           the clock reaches it (display_write_frame_custom, which the PPA
//           scales to fit the canvas), draws the overlay, reads input.
//
// Queues are single-producer single-consumer rings with plain loads and
// stores plus fences; each packet and frame belongs to one task at a time,
// so FFmpeg (built without threads) never sees two tasks on one object.
// Packets are copied out of the demuxer's buffers before they are queued,
// because some demuxers (MPEG-TS) allocate from pools that are not
// thread-safe in this build.
#include "papp_player.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "libavcodec/avcodec.h"
#include "libavformat/avformat.h"
#include "libavutil/avutil.h"
#include "libavutil/channel_layout.h"
#include "libavutil/pixdesc.h"
#include "libswresample/swresample.h"

#define VCAP 512                           // video packet queue
#define ACAP 1024                          // audio packet queue
#define NSLOTS 4                           // RGB565 frame slots (one on screen)
#define QUEUE_MAX_BYTES (8 * 1024 * 1024)  // both packet queues together
#define QUEUE_ENOUGH 60                    // packets per queue before the demuxer rests
#define AUDIO_CHUNK 1024                   // frames per audio_submit
#define IO_BUFFER 65536
#define LATE_DROP_US 90000                 // drop a decoded frame this late ...
#define SHOW_AT_LEAST_US 300000            // ... unless nothing was shown for this long
#define OVERLAY_HIDE_US 4000000
#define AUDIO_WAIT_US 1500000              // how long video waits for the audio clock

#define WHITE 0xFFFF
#define GREY RGB565(150, 150, 150)
#define ACCENT RGB565(56, 189, 248)
#define DARK RGB565(30, 30, 30)

enum { ST_OPENING, ST_READY, ST_FAILED };
enum { Q_PKT, Q_FLUSH, Q_EOF };

static inline void fence(void) { __atomic_thread_fence(__ATOMIC_SEQ_CST); }

// ── Queues ────────────────────────────────────────────────────────────────

typedef struct {
    AVPacket *pkt;
    int serial;
    int kind;
} qitem_t;

typedef struct {
    qitem_t *items;
    unsigned cap;
    volatile unsigned head, tail;          // written by the producer / the consumer
    volatile uint32_t bytes_in, bytes_out; // likewise; their difference is what waits
} pktq_t;

static bool q_init(pktq_t *q, unsigned cap)
{
    q->items = calloc(cap, sizeof(qitem_t));
    q->cap = cap;
    q->head = q->tail = 0;
    q->bytes_in = q->bytes_out = 0;
    return q->items != NULL;
}

static unsigned q_count(const pktq_t *q) { return q->head - q->tail; }
static uint32_t q_bytes(const pktq_t *q) { return q->bytes_in - q->bytes_out; }

static bool q_push(pktq_t *q, qitem_t it)
{
    const unsigned h = q->head;
    if (h - q->tail >= q->cap) {
        return false;
    }
    q->items[h % q->cap] = it;
    if (it.pkt != NULL) {
        q->bytes_in += (uint32_t)it.pkt->size;
    }
    fence();
    q->head = h + 1;
    return true;
}

static bool q_pop(pktq_t *q, qitem_t *out)
{
    const unsigned t = q->tail;
    if (t == q->head) {
        return false;
    }
    fence();
    *out = q->items[t % q->cap];
    if (out->pkt != NULL) {
        q->bytes_out += (uint32_t)out->pkt->size;
    }
    fence();
    q->tail = t + 1;
    return true;
}

// Frame slot indices: free slots (main -> video) and ready ones (video -> main).
typedef struct {
    int idx[NSLOTS + 1];
    volatile unsigned head, tail;
} idxq_t;

static bool iq_push(idxq_t *q, int v)
{
    const unsigned h = q->head;
    if (h - q->tail > NSLOTS) {
        return false;
    }
    q->idx[h % (NSLOTS + 1)] = v;
    fence();
    q->head = h + 1;
    return true;
}

static bool iq_peek(idxq_t *q, int *v)
{
    if (q->tail == q->head) {
        return false;
    }
    fence();
    *v = q->idx[q->tail % (NSLOTS + 1)];
    return true;
}

static void iq_drop(idxq_t *q)
{
    fence();
    q->tail = q->tail + 1;
}

static bool iq_pop(idxq_t *q, int *v)
{
    if (!iq_peek(q, v)) {
        return false;
    }
    iq_drop(q);
    return true;
}

static unsigned iq_count(const idxq_t *q) { return q->head - q->tail; }

typedef struct {
    uint16_t *px;
    size_t cap;
    int w, h;
    int64_t pts;
    int serial;
} slot_t;

// ── The player ────────────────────────────────────────────────────────────

typedef struct {
    char location[PAPP_APP_ARG_MAX];
    char title[128];
    volatile int stop;
    volatile int state;
    char error[160];

    papp_stream_t *stream;
    AVIOContext *avio;
    AVFormatContext *fmt;
    AVCodecContext *vctx, *actx;
    AVStream *vst, *ast;
    int vidx, aidx;
    int64_t start_us;
    int64_t duration_us;
    char vinfo[64], ainfo[64];

    pktq_t vq, aq;
    slot_t slots[NSLOTS];
    idxq_t freeq, readyq;

    volatile int serial;
    volatile int seek_req;
    volatile int seek_dir;
    int64_t seek_target;
    int64_t seek_from;
    volatile int paused;

    papp_lock_t *clk_lock;
    bool clk_valid;
    int clk_serial;
    int64_t clk_pts, clk_at;
    volatile int av_offset_ms;

    int out_rate;
    int64_t lat_us;
    volatile int lat_known;
    volatile int demux_eof_serial, video_eof_serial, audio_eof_serial;

    volatile int decoded, dropped_early, shown;
    int dropped_late;
    volatile int skip_level;
    int64_t lag_avg;
    int64_t skip_changed_us;
    int64_t last_convert_us;

    void *demux_task, *video_task, *audio_task;
    volatile int demux_done, video_done, audio_done;
} player_t;

static int64_t now_us(void) { return papp_time_us(); }

static int64_t to_us(int64_t ts, AVRational tb)
{
    return ts == AV_NOPTS_VALUE ? AV_NOPTS_VALUE : av_rescale_q(ts, tb, AV_TIME_BASE_Q);
}

// ── Clock ─────────────────────────────────────────────────────────────────
// A point (pts at time `at`) that runs on with the system clock; the audio
// task moves it after every audio_submit, or the main task sets it from the
// first frame when there is no audio. Invalid after a seek until one does.

static void clock_set(player_t *p, int64_t pts, int64_t at, int serial)
{
    papp_lock_take(p->clk_lock);
    if (serial == p->serial) {
        p->clk_pts = pts;
        p->clk_at = at;
        p->clk_valid = true;
        p->clk_serial = serial;
    }
    papp_lock_give(p->clk_lock);
}

static bool clock_get(player_t *p, int64_t *out)
{
    papp_lock_take(p->clk_lock);
    const bool ok = p->clk_valid && p->clk_serial == p->serial;
    int64_t v = p->clk_pts;
    if (ok && !p->paused) {
        v += now_us() - p->clk_at;
    }
    papp_lock_give(p->clk_lock);
    // A positive offset shows pictures later (for sound that lags).
    *out = v - (int64_t)p->av_offset_ms * 1000;
    return ok;
}

static void clock_pause(player_t *p, bool pause)
{
    papp_lock_take(p->clk_lock);
    const int64_t t = now_us();
    if (p->clk_valid && pause && !p->paused) {
        p->clk_pts += t - p->clk_at;
    }
    p->clk_at = t;
    p->paused = pause;
    papp_lock_give(p->clk_lock);
}

// ── FFmpeg glue ───────────────────────────────────────────────────────────

static void log_cb(void *avcl, int level, const char *fmt, va_list vl)
{
    (void)avcl;
    if (level > AV_LOG_WARNING) {
        return;
    }
    static int64_t window;
    static int count;
    const int64_t t = now_us();
    if (t - window > 1000000) {
        window = t;
        count = 0;
    }
    if (++count > 6) {
        return;  // decoders can report a broken stream for every macroblock
    }
    char line[200];
    vsnprintf(line, sizeof(line), fmt, vl);
    size_t n = strlen(line);
    while (n > 0 && (line[n - 1] == '\n' || line[n - 1] == '\r')) {
        line[--n] = '\0';
    }
    if (n > 0) {
        papp_log("ffmpeg: %s", line);
    }
}

static int io_read(void *opaque, uint8_t *buf, int size)
{
    player_t *p = opaque;
    const int n = papp_stream_read(p->stream, buf, size);
    if (n > 0) {
        return n;
    }
    return n == 0 ? AVERROR_EOF : AVERROR(EIO);
}

static int64_t io_seek(void *opaque, int64_t offset, int whence)
{
    player_t *p = opaque;
    const int64_t size = papp_stream_size(p->stream);
    if (whence & AVSEEK_SIZE) {
        return size >= 0 ? size : AVERROR(ENOSYS);
    }
    whence &= ~AVSEEK_FORCE;
    int64_t target;
    if (whence == SEEK_SET) {
        target = offset;
    } else if (whence == SEEK_CUR) {
        target = papp_stream_tell(p->stream) + offset;
    } else if (whence == SEEK_END && size >= 0) {
        target = size + offset;
    } else {
        return AVERROR(EINVAL);
    }
    return papp_stream_seek(p->stream, target) == 0 ? target : AVERROR(EIO);
}

static int interrupt_cb(void *opaque)
{
    return ((player_t *)opaque)->stop;
}

// A packet in a fresh buffer of its own (see the file comment).
static AVPacket *own_packet(const AVPacket *src)
{
    AVPacket *pkt = av_packet_alloc();
    if (pkt == NULL) {
        return NULL;
    }
    if (av_new_packet(pkt, src->size) < 0 || av_packet_copy_props(pkt, src) < 0) {
        av_packet_free(&pkt);
        return NULL;
    }
    memcpy(pkt->data, src->data, (size_t)src->size);
    pkt->stream_index = src->stream_index;
    return pkt;
}

static void push_wait(player_t *p, pktq_t *q, qitem_t it)
{
    while (!q_push(q, it)) {
        if (p->stop) {
            av_packet_free(&it.pkt);
            return;
        }
        papp_sleep_ms(10);
    }
}

static AVCodecContext *open_decoder(player_t *p, AVStream *st, char *info, size_t info_len)
{
    const AVCodec *codec = avcodec_find_decoder(st->codecpar->codec_id);
    if (codec == NULL) {
        snprintf(info, info_len, "%s (no decoder)", avcodec_get_name(st->codecpar->codec_id));
        return NULL;
    }
    AVCodecContext *ctx = avcodec_alloc_context3(codec);
    if (ctx == NULL || avcodec_parameters_to_context(ctx, st->codecpar) < 0) {
        avcodec_free_context(&ctx);
        return NULL;
    }
    ctx->pkt_timebase = st->time_base;
    ctx->thread_count = 1;
    ctx->flags2 |= AV_CODEC_FLAG2_FAST;
    // Big MJPEG pictures decode at half size (the panel is 1024x600 anyway).
    if (codec->id == AV_CODEC_ID_MJPEG && st->codecpar->width > 1280 && codec->max_lowres >= 1) {
        ctx->lowres = 1;
    }
    const int r = avcodec_open2(ctx, codec, NULL);
    if (r < 0) {
        snprintf(info, info_len, "%s (cannot open: %d)", codec->name, r);
        avcodec_free_context(&ctx);
        return NULL;
    }
    (void)p;
    if (st->codecpar->codec_type == AVMEDIA_TYPE_VIDEO) {
        snprintf(info, info_len, "%s %dx%d", codec->name, st->codecpar->width, st->codecpar->height);
    } else {
        snprintf(info, info_len, "%s %d Hz", codec->name, st->codecpar->sample_rate);
    }
    return ctx;
}

// ── vdemux ────────────────────────────────────────────────────────────────

static void video_task(void *arg);
static void audio_task(void *arg);

static bool queues_full(player_t *p)
{
    const unsigned vc = q_count(&p->vq), ac = q_count(&p->aq);
    if (vc >= VCAP - 4 || ac >= ACAP - 4 || q_bytes(&p->vq) + q_bytes(&p->aq) > QUEUE_MAX_BYTES) {
        return true;
    }
    const bool v_ok = p->vctx == NULL || vc > QUEUE_ENOUGH;
    const bool a_ok = p->actx == NULL || ac > QUEUE_ENOUGH;
    return v_ok && a_ok;
}

static bool demux_open(player_t *p)
{
    p->stream = papp_stream_open(p->location, &p->stop, p->error, sizeof(p->error));
    if (p->stream == NULL) {
        return false;
    }
    uint8_t *iobuf = av_malloc(IO_BUFFER);
    p->avio = iobuf != NULL ? avio_alloc_context(iobuf, IO_BUFFER, 0, p, io_read, NULL, io_seek) : NULL;
    if (p->avio == NULL) {
        av_free(iobuf);
        snprintf(p->error, sizeof(p->error), "out of memory");
        return false;
    }
    p->avio->seekable = papp_stream_seekable(p->stream) ? AVIO_SEEKABLE_NORMAL : 0;
    p->fmt = avformat_alloc_context();
    if (p->fmt == NULL) {
        snprintf(p->error, sizeof(p->error), "out of memory");
        return false;
    }
    p->fmt->pb = p->avio;
    p->fmt->flags |= AVFMT_FLAG_CUSTOM_IO;
    p->fmt->interrupt_callback.callback = interrupt_cb;
    p->fmt->interrupt_callback.opaque = p;
    p->fmt->probesize = 1024 * 1024;
    int r = avformat_open_input(&p->fmt, p->location, NULL, NULL);
    if (r < 0) {
        snprintf(p->error, sizeof(p->error), p->stop ? "stopped" : "not a video or sound file this player knows (%d)", r);
        return false;
    }
    r = avformat_find_stream_info(p->fmt, NULL);
    if (r < 0) {
        papp_log("find_stream_info: %d (going on)", r);
    }
    const AVDictionaryEntry *title = av_dict_get(p->fmt->metadata, "title", NULL, 0);
    if (title != NULL && title->value[0] != '\0') {
        snprintf(p->title, sizeof(p->title), "%s", title->value);
    }
    p->start_us = p->fmt->start_time != AV_NOPTS_VALUE ? p->fmt->start_time : 0;
    p->duration_us = p->fmt->duration != AV_NOPTS_VALUE ? p->fmt->duration : -1;

    p->vidx = av_find_best_stream(p->fmt, AVMEDIA_TYPE_VIDEO, -1, -1, NULL, 0);
    p->aidx = av_find_best_stream(p->fmt, AVMEDIA_TYPE_AUDIO, -1, p->vidx >= 0 ? p->vidx : -1, NULL, 0);
    if (p->vidx >= 0 && (p->fmt->streams[p->vidx]->disposition & AV_DISPOSITION_ATTACHED_PIC)) {
        p->vidx = -1;  // cover art of a song, not a video
    }
    if (p->vidx >= 0) {
        p->vst = p->fmt->streams[p->vidx];
        p->vctx = open_decoder(p, p->vst, p->vinfo, sizeof(p->vinfo));
    }
    if (p->aidx >= 0) {
        p->ast = p->fmt->streams[p->aidx];
        p->actx = open_decoder(p, p->ast, p->ainfo, sizeof(p->ainfo));
    }
    papp_log("opened %s: %s; video %s, audio %s, %lld s", p->fmt->iformat->name, p->title,
             p->vidx >= 0 ? p->vinfo : "none", p->aidx >= 0 ? p->ainfo : "none",
             (long long)(p->duration_us / 1000000));
    if (p->vctx == NULL && p->actx == NULL) {
        snprintf(p->error, sizeof(p->error), "nothing to play: video %s, audio %s", p->vidx >= 0 ? p->vinfo : "none",
                 p->aidx >= 0 ? p->ainfo : "none");
        return false;
    }
    // Discard what is not played.
    for (unsigned i = 0; i < p->fmt->nb_streams; i++) {
        if ((int)i != p->vidx && (int)i != p->aidx) {
            p->fmt->streams[i]->discard = AVDISCARD_ALL;
        }
    }
    if (p->actx != NULL) {
        const int rate = p->actx->sample_rate;
        p->out_rate = rate >= 8000 && rate <= 48000 ? rate : 48000;
    }
    return true;
}

static void do_seek(player_t *p)
{
    const int64_t target = p->seek_target;
    const int64_t from = p->seek_from;
    const int dir = p->seek_dir;
    p->seek_req = 0;
    const int64_t lo = dir > 0 ? from + 2 : INT64_MIN;
    const int64_t hi = dir < 0 ? from - 2 : INT64_MAX;
    int r = avformat_seek_file(p->fmt, -1, lo, target, hi, 0);
    if (r < 0) {
        r = avformat_seek_file(p->fmt, -1, INT64_MIN, target, INT64_MAX, 0);
    }
    if (r < 0) {
        papp_log("seek to %lld s failed (%d)", (long long)(target / 1000000), r);
        return;
    }
    const int serial = p->serial + 1;
    papp_lock_take(p->clk_lock);
    p->serial = serial;
    p->clk_valid = false;
    papp_lock_give(p->clk_lock);
    fence();
    if (p->vctx != NULL) {
        push_wait(p, &p->vq, (qitem_t){NULL, serial, Q_FLUSH});
    }
    if (p->actx != NULL) {
        push_wait(p, &p->aq, (qitem_t){NULL, serial, Q_FLUSH});
    }
}

static void demux_task(void *arg)
{
    player_t *p = arg;
    if (!demux_open(p)) {
        p->state = ST_FAILED;
        p->demux_done = 1;
        for (;;) {
            papp_svc->delay_ms(1000);
        }
    }
    // The decoders run on their own tasks: video on core 1 at the lowest
    // priority (it takes what the ESPHome loop leaves), audio next to this one.
    if (p->vctx != NULL &&
        papp_svc->task_create(video_task, "vvideo", 128 * 1024, p, 0, &p->video_task, 1) != 0) {
        p->video_task = NULL;
        papp_log("could not start the video task");
    }
    if (p->actx != NULL && papp_svc->task_create(audio_task, "vaudio", 64 * 1024, p, 6, &p->audio_task, 0) != 0) {
        p->audio_task = NULL;
        papp_log("could not start the audio task");
    }
    p->state = ST_READY;

    AVPacket *pkt = av_packet_alloc();
    bool eof = false;
    int errors = 0;
    while (!p->stop && pkt != NULL) {
        if (p->seek_req) {
            do_seek(p);
            eof = false;
            continue;
        }
        if (eof || queues_full(p)) {
            if (p->stream != NULL) {
                papp_stream_prefetch(p->stream);
            }
            papp_sleep_ms(10);
            continue;
        }
        const int r = av_read_frame(p->fmt, pkt);
        if (r < 0) {
            if (r != AVERROR_EOF && !avio_feof(p->fmt->pb) && ++errors < 20 && !p->stop) {
                papp_sleep_ms(20);  // a damaged spot or a network hiccup
                continue;
            }
            const int serial = p->serial;
            if (p->vctx != NULL) {
                push_wait(p, &p->vq, (qitem_t){NULL, serial, Q_EOF});
            }
            if (p->actx != NULL) {
                push_wait(p, &p->aq, (qitem_t){NULL, serial, Q_EOF});
            }
            p->demux_eof_serial = serial;
            eof = true;
            errors = 0;
            continue;
        }
        errors = 0;
        pktq_t *q = pkt->stream_index == p->vidx && p->vctx != NULL ? &p->vq
                    : pkt->stream_index == p->aidx && p->actx != NULL ? &p->aq
                                                                      : NULL;
        if (q != NULL) {
            AVPacket *own = own_packet(pkt);
            if (own != NULL) {
                push_wait(p, q, (qitem_t){own, p->serial, Q_PKT});
            }
        }
        av_packet_unref(pkt);
        papp_yield_if_due();
    }
    av_packet_free(&pkt);
    p->demux_done = 1;
    for (;;) {
        papp_svc->delay_ms(1000);  // deleted by the main task
    }
}

// ── vvideo ────────────────────────────────────────────────────────────────

// Decoder shortcuts while video runs behind: 1 skips the H.264 loop filter,
// 2 also skips frames nothing refers to (B-frames), 3 decodes key frames only.
static void adapt(player_t *p, int64_t lag)
{
    p->lag_avg = (p->lag_avg * 7 + lag) / 8;
    const int64_t t = now_us();
    if (t - p->skip_changed_us < 1500000) {
        return;
    }
    int level = p->skip_level;
    if (p->lag_avg > 2500000 && level < 3) {
        level = 3;
    } else if (p->lag_avg > 400000 && level < 2) {
        level = 2;
    } else if (p->lag_avg > 120000 && level < 1) {
        level = 1;
    } else if (p->lag_avg < 20000 && level > 0) {
        level--;
    }
    if (level != p->skip_level) {
        p->skip_level = level;
        p->skip_changed_us = t;
        p->vctx->skip_loop_filter = level >= 1 ? AVDISCARD_ALL : AVDISCARD_DEFAULT;
        p->vctx->skip_frame = level >= 3 ? AVDISCARD_NONKEY : (level >= 2 ? AVDISCARD_NONREF : AVDISCARD_DEFAULT);
        papp_log("video %lld ms behind: decoder shortcuts level %d", (long long)(p->lag_avg / 1000), level);
    }
}

static bool convert(slot_t *s, const AVFrame *fr)
{
    enum papp_yuv_layout layout;
    bool full = fr->color_range == AVCOL_RANGE_JPEG;
    switch (fr->format) {
    case AV_PIX_FMT_YUVJ420P:
        full = true;
        // fall through
    case AV_PIX_FMT_YUV420P:
        layout = PAPP_YUV420;
        break;
    case AV_PIX_FMT_YUVJ422P:
        full = true;
        // fall through
    case AV_PIX_FMT_YUV422P:
        layout = PAPP_YUV422;
        break;
    case AV_PIX_FMT_YUVJ444P:
        full = true;
        // fall through
    case AV_PIX_FMT_YUV444P:
        layout = PAPP_YUV444;
        break;
    case AV_PIX_FMT_GRAY8:
        layout = PAPP_GRAY;
        full = true;
        break;
    default: {
        static int warned = 0;
        if (!warned) {
            warned = 1;
            papp_log("pictures in %s cannot be shown (8-bit YUV 4:2:0/4:2:2/4:4:4 only)",
                     av_get_pix_fmt_name((enum AVPixelFormat)fr->format));
        }
        return false;
    }
    }
    // Whole multiples of 16 x 4 (the PPA and cache want whole lines): a few
    // pixels are cropped from the edges. Over 1280x720, every second pixel.
    const int step = fr->width > 1280 || fr->height > 720 ? 2 : 1;
    const int w = (fr->width / step) & ~15, h = (fr->height / step) & ~3;
    if (w < 16 || h < 4) {
        return false;
    }
    const int x0 = ((fr->width - w * step) / 2) & ~1, y0 = ((fr->height - h * step) / 2) & ~1;
    const size_t need = ((size_t)w * h * 2 + 127) & ~(size_t)127;
    if (s->cap < need) {
        free(s->px);
        s->px = papp_alloc_aligned(need, 128);
        s->cap = s->px != NULL ? need : 0;
        if (s->px == NULL) {
            return false;
        }
    }
    const bool bt709 = fr->colorspace == AVCOL_SPC_BT709 ||
                       (fr->colorspace == AVCOL_SPC_UNSPECIFIED && fr->height >= 720);
    papp_yuv_to_rgb565(s->px, w, (const uint8_t *const *)fr->data, fr->linesize, layout, x0, y0, w, h, step, full,
                       bt709);
    s->w = w;
    s->h = h;
    return true;
}

static void video_task(void *arg)
{
    player_t *p = arg;
    AVFrame *fr = av_frame_alloc();
    int serial = p->serial;
    int held = -1;
    int64_t next_pts = AV_NOPTS_VALUE;
    const AVRational fps = p->vst->avg_frame_rate.num > 0 ? p->vst->avg_frame_rate : p->vst->r_frame_rate;
    const int64_t frame_us = fps.num > 0 ? av_rescale(1000000, fps.den, fps.num) : 40000;
    while (!p->stop && fr != NULL) {
        qitem_t it;
        if (!q_pop(&p->vq, &it)) {
            papp_sleep_ms(10);
            continue;
        }
        if (it.kind == Q_FLUSH) {
            avcodec_flush_buffers(p->vctx);
            serial = it.serial;
            next_pts = AV_NOPTS_VALUE;
            p->lag_avg = 0;
            continue;
        }
        if (it.serial != p->serial) {
            av_packet_free(&it.pkt);
            continue;
        }
        int r = avcodec_send_packet(p->vctx, it.kind == Q_EOF ? NULL : it.pkt);
        av_packet_free(&it.pkt);
        if (r < 0 && r != AVERROR(EAGAIN) && r != AVERROR_EOF) {
            continue;  // a broken packet; the decoder carries on with the next
        }
        for (;;) {
            r = avcodec_receive_frame(p->vctx, fr);
            if (r == AVERROR_EOF) {
                p->video_eof_serial = serial;
                break;
            }
            if (r < 0) {
                break;
            }
            p->decoded++;
            int64_t pts = to_us(fr->best_effort_timestamp, p->vst->time_base);
            if (pts == AV_NOPTS_VALUE) {
                pts = next_pts != AV_NOPTS_VALUE ? next_pts : 0;
            }
            const int64_t dur = fr->duration > 0 ? to_us(fr->duration, p->vst->time_base) : frame_us;
            next_pts = pts + dur;

            int64_t clk;
            if (clock_get(p, &clk) && !p->paused && serial == p->serial) {
                const int64_t lag = clk - pts;
                adapt(p, lag);
                if (lag > LATE_DROP_US && now_us() - p->last_convert_us < SHOW_AT_LEAST_US) {
                    p->dropped_early++;
                    av_frame_unref(fr);
                    continue;
                }
            }
            // A free slot (one a failed conversion left over first: only the
            // main task may push to the free queue).
            int slot = held;
            held = -1;
            while (slot < 0 && !p->stop && serial == p->serial && !iq_pop(&p->freeq, &slot)) {
                slot = -1;
                papp_sleep_ms(10);
            }
            if (slot >= 0) {
                slot_t *s = &p->slots[slot];
                if (serial == p->serial && convert(s, fr)) {
                    s->pts = pts;
                    s->serial = serial;
                    p->last_convert_us = now_us();
                    iq_push(&p->readyq, slot);
                } else {
                    held = slot;
                }
            }
            // (No forced sleeps: at priority 0 this task shares core 1 with
            // its idle task, and everything else there comes first.)
            av_frame_unref(fr);
        }
    }
    av_frame_free(&fr);
    p->video_done = 1;
    for (;;) {
        papp_svc->delay_ms(1000);
    }
}

// ── vaudio ────────────────────────────────────────────────────────────────

static void audio_task(void *arg)
{
    player_t *p = arg;
    AVFrame *fr = av_frame_alloc();
    SwrContext *swr = NULL;
    AVChannelLayout in_layout = {0};
    int in_fmt = -1, in_rate = 0;
    const AVChannelLayout stereo = AV_CHANNEL_LAYOUT_STEREO;
    int16_t *buf = NULL;
    int buf_frames = 0;
    int serial = p->serial;
    int64_t next_pts = AV_NOPTS_VALUE;
    int64_t t_first = 0, submitted_us = 0;

    papp_svc->audio_init(p->out_rate);
    p->lat_us = 250000;  // until measured
    while (!p->stop && fr != NULL) {
        if (p->paused) {
            t_first = 0;
            submitted_us = 0;
            papp_sleep_ms(10);
            continue;
        }
        qitem_t it;
        if (!q_pop(&p->aq, &it)) {
            papp_sleep_ms(10);
            continue;
        }
        if (it.kind == Q_FLUSH) {
            avcodec_flush_buffers(p->actx);
            if (swr != NULL) {
                swr_close(swr);
                swr_init(swr);
            }
            serial = it.serial;
            next_pts = AV_NOPTS_VALUE;
            t_first = 0;  // the speaker's buffer is measured again (until known)
            submitted_us = 0;
            continue;
        }
        if (it.serial != p->serial) {
            av_packet_free(&it.pkt);
            continue;
        }
        int r = avcodec_send_packet(p->actx, it.kind == Q_EOF ? NULL : it.pkt);
        av_packet_free(&it.pkt);
        if (r < 0 && r != AVERROR(EAGAIN) && r != AVERROR_EOF) {
            continue;
        }
        while (!p->stop) {
            r = avcodec_receive_frame(p->actx, fr);
            if (r == AVERROR_EOF) {
                p->audio_eof_serial = serial;
                break;
            }
            if (r < 0) {
                break;
            }
            int64_t pts = to_us(fr->best_effort_timestamp, p->ast->time_base);
            if (pts == AV_NOPTS_VALUE) {
                pts = next_pts != AV_NOPTS_VALUE ? next_pts : 0;
            }
            next_pts = pts + (int64_t)fr->nb_samples * 1000000 / (fr->sample_rate > 0 ? fr->sample_rate : 48000);

            // (Re)configure the converter when the input changes.
            if (swr == NULL || fr->format != in_fmt || fr->sample_rate != in_rate ||
                av_channel_layout_compare(&fr->ch_layout, &in_layout) != 0) {
                swr_free(&swr);
                av_channel_layout_uninit(&in_layout);
                av_channel_layout_copy(&in_layout, &fr->ch_layout);
                in_fmt = fr->format;
                in_rate = fr->sample_rate;
                if (swr_alloc_set_opts2(&swr, &stereo, AV_SAMPLE_FMT_S16, p->out_rate, &in_layout,
                                        (enum AVSampleFormat)in_fmt, in_rate, 0, NULL) < 0 ||
                    swr_init(swr) < 0) {
                    papp_log("cannot convert this sound (%d Hz, %d channels)", in_rate, in_layout.nb_channels);
                    swr_free(&swr);
                    av_frame_unref(fr);
                    continue;
                }
            }
            const int max_out = swr_get_out_samples(swr, fr->nb_samples);
            if (max_out > buf_frames) {
                free(buf);
                buf = malloc((size_t)max_out * 2 * sizeof(int16_t));
                buf_frames = buf != NULL ? max_out : 0;
            }
            int n = buf != NULL ? swr_convert(swr, (uint8_t *const *)&buf, buf_frames,
                                              (const uint8_t *const *)fr->extended_data, fr->nb_samples)
                                : 0;
            av_frame_unref(fr);
            for (int off = 0; off < n && !p->stop && serial == p->serial && !p->paused;) {
                const int c = n - off < AUDIO_CHUNK ? n - off : AUDIO_CHUNK;
                const int64_t t0 = now_us();
                papp_svc->audio_submit(buf + off * 2, c);
                const int64_t t1 = now_us();
                off += c;
                // The speaker's buffer: once a submit has to wait, it is full,
                // and what was handed over minus what has played is its size.
                if (t_first == 0) {
                    t_first = t0;
                }
                submitted_us += (int64_t)c * 1000000 / p->out_rate;
                if (!p->lat_known && t1 - t0 > 8000 && submitted_us > 50000) {
                    int64_t lat = submitted_us - (t1 - t_first);
                    lat = lat < 30000 ? 30000 : (lat > 1500000 ? 1500000 : lat);
                    p->lat_us = lat;
                    p->lat_known = 1;
                    papp_log("audio buffer about %lld ms", (long long)(lat / 1000));
                }
                clock_set(p, pts + (int64_t)off * 1000000 / p->out_rate - p->lat_us, t1, serial);
            }
        }
    }
    swr_free(&swr);
    av_channel_layout_uninit(&in_layout);
    free(buf);
    av_frame_free(&fr);
    p->audio_done = 1;
    for (;;) {
        papp_svc->delay_ms(1000);
    }
}

// ── Screens ───────────────────────────────────────────────────────────────

static papp_canvas_t canvas_fb(int cw, int ch)
{
    papp_canvas_t c = {papp_svc->display_get_framebuffer(), cw, ch, cw};
    return c;
}

static void format_time(char *out, size_t len, int64_t us)
{
    if (us < 0) {
        snprintf(out, len, "--:--");
        return;
    }
    const long s = (long)(us / 1000000);
    if (s >= 3600) {
        snprintf(out, len, "%ld:%02ld:%02ld", s / 3600, (s / 60) % 60, s % 60);
    } else {
        snprintf(out, len, "%ld:%02ld", s / 60, s % 60);
    }
}

static void center_text(const papp_canvas_t *c, int y, const char *text, int scale, uint16_t color)
{
    int w = papp_text_width(text, scale);
    if (w > c->w - 16) {
        papp_text_fit(c, 8, y, text, scale, color, c->w - 16);
        return;
    }
    papp_text(c, (c->w - w) / 2, y, text, scale, color);
}

bool papp_message(const char *title, const char *text, int cw, int ch, int ms, papp_input_t *in)
{
    papp_log("%s: %s", title, text);
    const papp_canvas_t c = canvas_fb(cw, ch);
    if (c.px == NULL) {
        return true;
    }
    papp_fill(&c, 0, 0, cw, ch, 0);
    center_text(&c, ch / 2 - 60, title, 3, WHITE);
    // The text in lines of what fits at scale 2.
    const int per_line = (cw - 40) / 16;
    const int len = (int)strlen(text);
    int y = ch / 2;
    for (int at = 0; at < len && y < ch - 60; y += 36) {
        char line[160];
        int n = len - at < per_line ? len - at : per_line;
        if (n > (int)sizeof(line) - 1) {
            n = (int)sizeof(line) - 1;
        }
        if (at + n < len) {
            int cut = n;
            while (cut > n / 2 && text[at + cut] != ' ') {
                cut--;
            }
            if (cut > n / 2) {
                n = cut;
            }
        }
        memcpy(line, text + at, (size_t)n);
        line[n] = '\0';
        center_text(&c, y, line, 2, GREY);
        at += n;
        while (at < len && text[at] == ' ') {
            at++;
        }
    }
    center_text(&c, ch - 50, "Tap or press a button", 2, GREY);
    papp_svc->display_flush();
    const int64_t until = now_us() + (int64_t)ms * 1000;
    papp_input_poll(in);
    while (now_us() < until) {
        papp_sleep_ms(20);
        papp_input_poll(in);
        if (in->quit_app) {
            return false;
        }
        if (in->tap || in->nkeys > 0 || papp_button(in, PAPP_INPUT_A, 0) || papp_button(in, PAPP_INPUT_B, 0) ||
            papp_button(in, PAPP_INPUT_START, 0)) {
            break;
        }
    }
    return true;
}

// ── Overlay ───────────────────────────────────────────────────────────────
// Drawn onto a copy of the frame (at the video's own size, which the PPA
// then scales), or onto the canvas for sound only. Sizes are in screen
// pixels divided by the scale, so it looks the same for every video size.

typedef struct {
    int u, fs, lh;
    int top_h, bot_y, bar_x, bar_y, bar_w, bar_h;
    int btn_x, btn_y, btn_r;
    int back_w;
} layout_t;

static void make_layout(layout_t *L, int w, int h, float scale)
{
    L->fs = (int)(1.3f / scale + 0.5f);
    if (L->fs < 1) {
        L->fs = 1;
    }
    L->u = (int)(8.0f / scale + 0.5f);
    if (L->u < 2) {
        L->u = 2;
    }
    L->lh = 16 * L->fs;
    L->top_h = L->lh + 2 * L->u;
    L->bot_y = h - (L->lh + 6 * L->u);
    L->bar_x = 2 * L->u;
    L->bar_w = w - 4 * L->u;
    L->bar_h = L->u;
    L->bar_y = h - 2 * L->u - L->bar_h;
    L->btn_r = (w < h ? w : h) / 8;
    L->btn_x = w / 2;
    L->btn_y = h / 2;
    L->back_w = 7 * 8 * L->fs + 2 * L->u;
}

static void draw_play_button(const papp_canvas_t *c, const layout_t *L, bool paused)
{
    const int r = L->btn_r;
    papp_dim(c, L->btn_x - r, L->btn_y - r, 2 * r, 2 * r);
    papp_dim(c, L->btn_x - r, L->btn_y - r, 2 * r, 2 * r);
    if (paused) {
        // A play triangle: rows narrowing to a point on the right.
        const int th = r, tw = r * 7 / 8;
        for (int dy = -th / 2; dy <= th / 2; dy++) {
            const int len = tw * (th / 2 - (dy < 0 ? -dy : dy)) / (th / 2 > 0 ? th / 2 : 1);
            papp_fill(c, L->btn_x - tw / 3, L->btn_y + dy, len, 1, WHITE);
        }
    } else {
        const int bw = r / 4, bh = r;
        papp_fill(c, L->btn_x - bw - bw / 2, L->btn_y - bh / 2, bw, bh, WHITE);
        papp_fill(c, L->btn_x + bw / 2, L->btn_y - bh / 2, bw, bh, WHITE);
    }
}

static void draw_overlay(player_t *p, const papp_canvas_t *c, const layout_t *L, int64_t pos, const char *stats)
{
    papp_dim(c, 0, 0, c->w, L->top_h);
    papp_text(c, L->u, L->u, "< Back", L->fs, WHITE);
    const int stats_w = papp_text_width(stats, L->fs);
    papp_text_fit(c, L->back_w + L->u, L->u, p->title, L->fs, WHITE, c->w - L->back_w - stats_w - 4 * L->u);
    papp_text(c, c->w - stats_w - L->u, L->u, stats, L->fs, GREY);

    papp_dim(c, 0, L->bot_y, c->w, c->h - L->bot_y);
    char a[16], b[16], line[48];
    format_time(a, sizeof(a), pos);
    format_time(b, sizeof(b), p->duration_us);
    snprintf(line, sizeof(line), "%s / %s%s", a, b, p->paused ? "   paused" : "");
    papp_text(c, L->bar_x, L->bot_y + L->u, line, L->fs, WHITE);
    if (p->av_offset_ms != 0) {
        char off[24];
        snprintf(off, sizeof(off), "A/V %+d ms", p->av_offset_ms);
        papp_text(c, c->w - papp_text_width(off, L->fs) - L->u, L->bot_y + L->u, off, L->fs, GREY);
    }
    papp_fill(c, L->bar_x, L->bar_y, L->bar_w, L->bar_h, DARK);
    if (p->duration_us > 0) {
        int64_t done = (int64_t)L->bar_w * (pos < 0 ? 0 : pos) / p->duration_us;
        if (done > L->bar_w) {
            done = L->bar_w;
        }
        papp_fill(c, L->bar_x, L->bar_y, (int)done, L->bar_h, ACCENT);
        papp_fill(c, L->bar_x + (int)done - L->bar_h, L->bar_y - L->bar_h / 2, 2 * L->bar_h, 2 * L->bar_h, WHITE);
    }
    draw_play_button(c, L, p->paused);
}

// ── papp_play ─────────────────────────────────────────────────────────────

// The scale that fits a w x h picture in the canvas, in the 1/16 steps the
// PPA scales in (a finer one is rounded down by the driver, and the picture
// would not be centred).
static float fit_scale(int w, int h, int cw, int ch)
{
    const float fit = fminf((float)cw / w, (float)ch / h);
    const float q = floorf(fit * 16.0f) / 16.0f;
    return q > 0.0f ? q : fit;
}

static void player_free(player_t *p)
{
    p->stop = 1;
    const int64_t until = now_us() + 3000000;
    while (now_us() < until && ((p->demux_task && !p->demux_done) || (p->video_task && !p->video_done) ||
                                (p->audio_task && !p->audio_done))) {
        papp_sleep_ms(10);
    }
    if (!((!p->demux_task || p->demux_done) && (!p->video_task || p->video_done) &&
          (!p->audio_task || p->audio_done))) {
        papp_log("a player task did not stop in time; stopping it anyway");
    }
    // The demuxer may have started the decoders just now: read the handles
    // after it has finished.
    void *tasks[3] = {p->video_task, p->audio_task, p->demux_task};
    for (int i = 0; i < 3; i++) {
        if (tasks[i] != NULL) {
            papp_svc->task_delete(tasks[i]);
        }
    }
    qitem_t it;
    while (p->vq.items != NULL && q_pop(&p->vq, &it)) {
        av_packet_free(&it.pkt);
    }
    while (p->aq.items != NULL && q_pop(&p->aq, &it)) {
        av_packet_free(&it.pkt);
    }
    free(p->vq.items);
    free(p->aq.items);
    for (int i = 0; i < NSLOTS; i++) {
        free(p->slots[i].px);
    }
    avcodec_free_context(&p->vctx);
    avcodec_free_context(&p->actx);
    avformat_close_input(&p->fmt);
    if (p->avio != NULL) {
        av_freep(&p->avio->buffer);
        avio_context_free(&p->avio);
    }
    papp_stream_close(p->stream);
    papp_lock_free(p->clk_lock);
    free(p);
}

static void title_from_location(char *out, size_t len, const char *location)
{
    const char *end = location + strcspn(location, "?#");
    const char *start = end;
    while (start > location && start[-1] != '/') {
        start--;
    }
    if (start == end) {
        start = location;
    }
    size_t o = 0;
    for (const char *s = start; s < end && o + 1 < len; s++) {
        if (s[0] == '%' && s + 2 < end && s[1] == '2' && s[2] == '0') {
            out[o++] = ' ';
            s += 2;
        } else {
            out[o++] = *s;
        }
    }
    out[o] = '\0';
}

static void opening_screen(player_t *p, int cw, int ch, int64_t since)
{
    const papp_canvas_t c = canvas_fb(cw, ch);
    if (c.px == NULL) {
        return;
    }
    papp_fill(&c, 0, 0, cw, ch, 0);
    center_text(&c, ch / 2 - 40, p->title, 2, WHITE);
    char line[64];
    const int dots = (int)((now_us() - since) / 400000) % 4;
    snprintf(line, sizeof(line), "Opening%.*s", dots, "...");
    center_text(&c, ch / 2 + 10, line, 2, GREY);
    center_text(&c, ch - 50, "B or Esc: back", 2, GREY);
    papp_svc->display_flush();
}

enum papp_play_result papp_play(const char *location, int cw, int ch, papp_input_t *in)
{
    av_log_set_callback(log_cb);
    av_log_set_level(AV_LOG_WARNING);
    player_t *p = calloc(1, sizeof(*p));
    if (p == NULL) {
        return PLAY_FAILED;
    }
    snprintf(p->location, sizeof(p->location), "%s", location);
    title_from_location(p->title, sizeof(p->title), location);
    p->vidx = p->aidx = -1;
    p->duration_us = -1;
    p->demux_eof_serial = p->video_eof_serial = p->audio_eof_serial = -1;
    p->clk_lock = papp_lock_new();
    if (p->clk_lock == NULL || !q_init(&p->vq, VCAP) || !q_init(&p->aq, ACAP)) {
        player_free(p);
        return PLAY_FAILED;
    }
    for (int i = 0; i < NSLOTS; i++) {
        iq_push(&p->freeq, i);
    }
    papp_log("play %.200s", location);
    if (papp_svc->task_create(demux_task, "vdemux", 64 * 1024, p, 4, &p->demux_task, 0) != 0) {
        p->demux_task = NULL;
        player_free(p);
        papp_message("Cannot play", "could not start the player task", cw, ch, 4000, in);
        return PLAY_FAILED;
    }

    // Opening (a network stream can take a while).
    const int64_t open_since = now_us();
    int64_t last_draw = 0;
    while (p->state == ST_OPENING) {
        papp_input_poll(in);
        if (in->quit_app || papp_button(in, PAPP_INPUT_B, 0) || papp_key(in, 27)) {
            const bool quit = in->quit_app;
            player_free(p);
            return quit ? PLAY_QUIT_APP : PLAY_BACK;
        }
        if (now_us() - last_draw > 400000) {
            opening_screen(p, cw, ch, open_since);
            last_draw = now_us();
        }
        papp_sleep_ms(20);
    }
    if (p->state == ST_FAILED) {
        char text[200];
        snprintf(text, sizeof(text), "%s", p->error[0] ? p->error : "unknown error");
        char title[64];
        snprintf(title, sizeof(title), "Cannot play");
        player_free(p);
        return papp_message(title, text, cw, ch, 6000, in) ? PLAY_FAILED : PLAY_QUIT_APP;
    }

    const bool has_video = p->vctx != NULL && p->video_task != NULL;
    const bool has_audio = p->actx != NULL && p->audio_task != NULL;
    uint16_t *ovl = NULL;
    size_t ovl_cap = 0;
    int shown = -1;               // slot on screen
    int shown_serial = -1;
    int64_t shown_pts = AV_NOPTS_VALUE;
    int64_t serial_since = now_us();
    int last_serial = p->serial;
    bool overlay = true;
    int64_t overlay_at = now_us();
    bool redraw = true;
    int64_t last_redraw = 0;
    int64_t stats_at = now_us();
    int stats_decoded = 0, stats_shown = 0;
    char stats[48] = "";
    int64_t ended_at = 0;
    int64_t last_toggle = 0;
    const int64_t ready_at = now_us();
    enum papp_play_result result = PLAY_ENDED;
    papp_svc->display_clear(0);

    for (;;) {
        const int64_t t = now_us();
        if (papp_fatal_hit) {
            result = PLAY_FAILED;
            break;
        }
        papp_input_poll(in);
        if (in->quit_app) {
            result = PLAY_QUIT_APP;
            break;
        }
        if (papp_button(in, PAPP_INPUT_B, 0) || papp_key(in, 27)) {
            result = PLAY_BACK;
            break;
        }

        // Where playback is, for seeking and the overlay.
        int64_t clk = AV_NOPTS_VALUE;
        const bool have_clock = clock_get(p, &clk);
        const int64_t pos_abs = have_clock ? clk : shown_pts;
        const int64_t pos = pos_abs != AV_NOPTS_VALUE ? pos_abs - p->start_us : -1;

        // ── Input ──
        int64_t seek = 0;   // relative, us
        int64_t seek_to = -1;  // absolute position from the bar
        bool toggle = papp_button(in, PAPP_INPUT_A, 0) || papp_key(in, ' ') || papp_key(in, 'k') ||
                      papp_key(in, 13) || papp_button(in, PAPP_INPUT_START, 0);
        if (papp_button(in, PAPP_INPUT_LEFT, 400) || papp_button(in, PAPP_INPUT_L, 400)) {
            seek -= 10000000;
        }
        if (papp_button(in, PAPP_INPUT_RIGHT, 400) || papp_button(in, PAPP_INPUT_R, 400)) {
            seek += 10000000;
        }
        if (papp_button(in, PAPP_INPUT_UP, 400)) {
            seek += 60000000;
        }
        if (papp_button(in, PAPP_INPUT_DOWN, 400)) {
            seek -= 60000000;
        }
        if (papp_key(in, 'o') || papp_button(in, PAPP_INPUT_Y, 0) || papp_button(in, PAPP_INPUT_SELECT, 0)) {
            overlay = !overlay;
            overlay_at = t;
            redraw = true;
        }
        if (papp_key(in, ']')) {
            p->av_offset_ms += 50;
            overlay = true;
            overlay_at = t;
            redraw = true;
        }
        if (papp_key(in, '[')) {
            p->av_offset_ms -= 50;
            overlay = true;
            overlay_at = t;
            redraw = true;
        }
        if (in->tap) {
            // Canvas -> the picture's coordinates (or the canvas, for sound).
            int vw = cw, vh = ch;
            float sc = 1.0f;
            int ox = 0, oy = 0;
            if (has_video && shown >= 0) {
                const slot_t *s = &p->slots[shown];
                vw = s->w;
                vh = s->h;
                sc = fit_scale(vw, vh, cw, ch);
                int out_w = (int)(vw * sc + 0.5f), out_h = (int)(vh * sc + 0.5f);
                out_w = out_w > cw ? cw : out_w;
                out_h = out_h > ch ? ch : out_h;
                ox = (cw - out_w) / 2;
                oy = (ch - out_h) / 2;
            }
            const int x = (int)((in->tap_x - ox) / sc), y = (int)((in->tap_y - oy) / sc);
            layout_t L;
            make_layout(&L, vw, vh, sc);
            if (!overlay && has_video) {
                overlay = true;
                redraw = true;
            } else if (y >= L.bot_y - L.u && p->duration_us > 0) {
                int64_t f = (int64_t)(x - L.bar_x) * 1000 / (L.bar_w > 0 ? L.bar_w : 1);
                f = f < 0 ? 0 : (f > 1000 ? 1000 : f);
                seek_to = p->duration_us * f / 1000;
            } else if (y < L.top_h && x < L.back_w) {
                result = PLAY_BACK;
                break;
            } else if (abs(x - L.btn_x) <= L.btn_r && abs(y - L.btn_y) <= L.btn_r) {
                toggle = true;
            } else if (has_video) {
                overlay = false;
                redraw = true;
            }
            overlay_at = t;
        }
        // Space and Enter arrive both as a key and as a held button: one toggle.
        if (toggle && t - last_toggle > 300000) {
            last_toggle = t;
            clock_pause(p, !p->paused);
            overlay = true;
            overlay_at = t;
            redraw = true;
        }
        if ((seek != 0 || seek_to >= 0) && papp_stream_seekable(p->stream) && pos >= 0) {
            int64_t target = seek_to >= 0 ? seek_to : pos + seek;
            if (p->duration_us > 0 && target > p->duration_us - 1000000) {
                target = p->duration_us - 1000000;
            }
            if (target < 0) {
                target = 0;
            }
            p->seek_from = pos_abs;
            p->seek_dir = target > pos ? 1 : -1;
            p->seek_target = target + p->start_us;
            fence();
            p->seek_req = 1;
            overlay = true;
            overlay_at = t;
            redraw = true;
        }
        if (overlay && !p->paused && t - overlay_at > OVERLAY_HIDE_US && has_video) {
            overlay = false;
            redraw = true;
        }
        if (p->serial != last_serial) {
            last_serial = p->serial;
            serial_since = t;
            ended_at = 0;
        }

        // Once a second: the rates for the overlay.
        if (t - stats_at >= 1000000) {
            const float secs = (float)(t - stats_at) / 1e6f;
            const int dec = p->decoded, shw = p->shown;
            if (has_video) {
                snprintf(stats, sizeof(stats), "%.1f fps  dec %.1f%s", (shw - stats_shown) / secs,
                         (dec - stats_decoded) / secs, p->skip_level ? "  skip" : "");
            } else {
                snprintf(stats, sizeof(stats), "%s", p->ainfo);
            }
            stats_decoded = dec;
            stats_shown = shw;
            stats_at = t;
            redraw = redraw || overlay;
        }

        // ── Frames ──
        bool presented = false;
        int idx;
        while (has_video && iq_peek(&p->readyq, &idx)) {
            slot_t *s = &p->slots[idx];
            if (s->serial != p->serial) {
                iq_drop(&p->readyq);
                iq_push(&p->freeq, idx);
                continue;
            }
            int64_t now_clk;
            bool ok = clock_get(p, &now_clk);
            if (!ok) {
                // No clock yet (start, seek): with sound, wait for it a
                // little, showing the first picture meanwhile.
                const bool first = shown_serial != p->serial;
                if (has_audio && !first && t - serial_since < AUDIO_WAIT_US && p->audio_eof_serial != p->serial) {
                    break;
                }
                if (!has_audio || !first || t - serial_since >= AUDIO_WAIT_US) {
                    clock_set(p, s->pts, t, p->serial);
                }
                now_clk = s->pts;
            } else if (s->pts - now_clk > 8000 && shown_serial == p->serial) {
                break;  // not yet
            } else if (now_clk - s->pts > 40000 && iq_count(&p->readyq) > 1) {
                // Late, and the next one is waiting: skip this one.
                int next_idx = p->readyq.idx[(p->readyq.tail + 1) % (NSLOTS + 1)];
                if (p->slots[next_idx].serial == p->serial && p->slots[next_idx].pts <= now_clk + 8000) {
                    iq_drop(&p->readyq);
                    iq_push(&p->freeq, idx);
                    p->dropped_late++;
                    continue;
                }
            }
            iq_drop(&p->readyq);
            if (shown >= 0) {
                iq_push(&p->freeq, shown);
            }
            shown = idx;
            shown_serial = s->serial;
            shown_pts = s->pts;
            presented = true;
            p->shown++;
            break;
        }

        // ── Drawing ──
        if (has_video && shown >= 0 && (presented || (redraw && t - last_redraw > 30000))) {
            const slot_t *s = &p->slots[shown];
            const float sc = fit_scale(s->w, s->h, cw, ch);
            const uint16_t *out = s->px;
            if (overlay) {
                const size_t bytes = (size_t)s->w * s->h * 2;
                const size_t need = (bytes + 127) & ~(size_t)127;
                if (ovl_cap < need) {
                    free(ovl);
                    ovl = papp_alloc_aligned(need, 128);
                    ovl_cap = ovl != NULL ? need : 0;
                }
                if (ovl != NULL) {
                    memcpy(ovl, s->px, bytes);
                    const papp_canvas_t c = {ovl, s->w, s->h, s->w};
                    layout_t L;
                    make_layout(&L, s->w, s->h, sc);
                    const int64_t shown_pos = shown_pts != AV_NOPTS_VALUE ? shown_pts - p->start_us : pos;
                    draw_overlay(p, &c, &L, shown_pos, stats);
                    out = ovl;
                }
            }
            papp_svc->display_write_frame_custom(out, (uint16_t)s->w, (uint16_t)s->h, sc, false);
            redraw = false;
            last_redraw = t;
        } else if ((!has_video || (shown < 0 && t - ready_at > 2000000)) && (redraw || t - last_redraw > 250000)) {
            // Sound only (or pictures that cannot be shown): the overlay on
            // the canvas, always shown.
            const papp_canvas_t c = canvas_fb(cw, ch);
            if (c.px != NULL) {
                papp_fill(&c, 0, 0, cw, ch, 0);
                layout_t L;
                make_layout(&L, cw, ch, 1.0f);
                center_text(&c, ch / 3 - 30, p->title, 2, WHITE);
                center_text(&c, ch / 3 + 10, p->ainfo, 2, GREY);
                draw_overlay(p, &c, &L, pos, stats);
                papp_svc->display_flush();
            }
            redraw = false;
            last_redraw = t;
        }

        // ── The end ──
        const int serial = p->serial;
        const bool demux_done = p->demux_eof_serial == serial;
        const bool video_done = !has_video || (p->video_eof_serial == serial && iq_count(&p->readyq) == 0);
        const bool audio_done = !has_audio || p->audio_eof_serial == serial;
        if (demux_done && video_done && audio_done && !p->paused) {
            if (ended_at == 0) {
                ended_at = t;
            }
            // Let the speaker play out what it still holds.
            if (t - ended_at > (has_audio ? p->lat_us : 0) + 200000) {
                result = PLAY_ENDED;
                break;
            }
        }

        // Sleep until the next frame is due (one tick at least).
        papp_sleep_ms(10);
    }

    papp_log("played %d frames, dropped %d before and %d after conversion; result %d", p->shown, p->dropped_early,
             p->dropped_late, (int)result);
    player_free(p);
    free(ovl);
    return result;
}
