// Input streams for video: files on the card through the loader's file
// services, and http:// / https:// through its net_tcp_* and net_tls_*
// services (the loader does the TLS handshake and checks the certificate).
//
// HTTP is HTTP/1.1 with Range requests: the first request asks for bytes=0-
// and a 206 answer with a Content-Range total makes the stream seekable. A
// read outside what is buffered starts a new request at that offset (a short
// jump forward just reads on). Redirects are followed, chunked bodies decoded.
// Everything received goes through a read-ahead ring in PSRAM, which also
// keeps some of what was already read, so the small backward jumps demuxers
// make cost nothing. The demuxer task tops the ring up while its packet
// queues are full (papp_stream_prefetch).
//
// All calls block the calling task (the demuxer's), sleeping a scheduler
// tick while the network has nothing, and give up when *abort_flag is set.
#define _GNU_SOURCE 1  // strcasestr
#include "papp_port.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#define RING_BYTES (4 * 1024 * 1024)       // read-ahead ring in PSRAM
#define KEEP_BEHIND (512 * 1024)           // bytes kept behind the read position
#define FILL_CHUNK (64 * 1024)             // what a read that must wait asks for
#define SKIP_TCP (256 * 1024)              // read on instead of reconnecting, http
#define SKIP_TLS (1024 * 1024)             // the same for https (a handshake is slow)
#define CONNECT_TIMEOUT_US 15000000LL
#define TLS_CONNECT_TIMEOUT_US 40000000LL
#define IDLE_TIMEOUT_US 20000000LL
#define MAX_REDIRECTS 5
#define HEADER_MAX 8192

enum { CH_SIZE, CH_EXT, CH_DATA, CH_DATA_END, CH_TRAILER, CH_TRAILER_LINE };

struct papp_stream {
    bool http;
    volatile int *abort_flag;
    int64_t pos;           // where the next read starts
    int64_t size;          // -1 unknown

    // file
    void *fp;

    // http
    bool tls;
    bool seekable;
    char host[256];
    uint16_t port;
    char *target;          // request target (path and query)
    int h;                 // connection handle, -1 none
    uint32_t ip;           // last looked-up address of host (TCP)
    int64_t conn_pos;      // offset of the next body byte on h
    bool conn_eof;         // this response's body is over
    int64_t body_left;     // Content-Length left, -1 unknown
    bool chunked;
    int chunk_state;
    int64_t chunk_left;
    uint8_t *ring;
    int64_t ring_start, ring_end;  // offsets held in the ring
    uint8_t raw[16384];    // received but not yet decoded
    int raw_off, raw_len;
};

static bool aborted(papp_stream_t *s)
{
    return s->abort_flag != NULL && *s->abort_flag;
}

// ── Transport (plain TCP or the loader's TLS) ────────────────────────────

static void tp_close(papp_stream_t *s)
{
    if (s->h < 0) {
        return;
    }
    if (s->tls) {
        papp_svc->net_tls_close(s->h);
    } else {
        papp_svc->net_udp_close(s->h);  // closes TCP handles too
    }
    s->h = -1;
}

// > 0 bytes, 0 nothing yet, -1 closed by the peer, -2 failed.
static int tp_recv(papp_stream_t *s, uint8_t *buf, int len)
{
    if (s->tls) {
        const int n = papp_svc->net_tls_recv(s->h, buf, len);
        if (n > 0) {
            return n;
        }
        if (n == 0) {
            return 0;
        }
        return papp_svc->net_tls_status(s->h) == 1 ? -1 : -2;
    }
    const int n = papp_svc->net_tcp_recv(s->h, buf, len);
    if (n > 0) {
        return n;
    }
    if (n == -2) {
        return 0;
    }
    return n == 0 ? -1 : -2;
}

static bool tp_send_all(papp_stream_t *s, const char *data, int len)
{
    const int64_t until = papp_time_us() + IDLE_TIMEOUT_US;
    int off = 0;
    while (off < len) {
        const int n = s->tls ? papp_svc->net_tls_send(s->h, data + off, len - off)
                             : papp_svc->net_tcp_send(s->h, data + off, len - off);
        if (n < 0) {
            return false;
        }
        if (n == 0) {
            if (aborted(s) || papp_time_us() > until) {
                return false;
            }
            papp_sleep_ms(10);
            continue;
        }
        off += n;
    }
    return true;
}

static bool parse_ipv4(const char *text, uint32_t *ip)
{
    unsigned a[4];
    char tail;
    if (sscanf(text, "%3u.%3u.%3u.%3u%c", &a[0], &a[1], &a[2], &a[3], &tail) != 4) {
        return false;
    }
    for (int i = 0; i < 4; i++) {
        if (a[i] > 255) {
            return false;
        }
    }
    *ip = (a[0] << 24) | (a[1] << 16) | (a[2] << 8) | a[3];
    return true;
}

static bool tp_open(papp_stream_t *s, char *err, size_t err_len)
{
    tp_close(s);
    if (s->tls) {
        if (papp_svc->net_tls_connect == NULL) {
            snprintf(err, err_len, "this loader has no TLS for apps: https:// needs a newer loader");
            return false;
        }
        s->h = papp_svc->net_tls_connect(s->host, s->port);
        if (s->h < 0) {
            snprintf(err, err_len, "the loader could not start a TLS connection (no free session?)");
            return false;
        }
        const int64_t until = papp_time_us() + TLS_CONNECT_TIMEOUT_US;
        for (;;) {
            const int st = papp_svc->net_tls_status(s->h);
            if (st > 0) {
                return true;
            }
            if (st < 0) {
                snprintf(err, err_len, "secure connection to %.60s failed (see the device log)", s->host);
                tp_close(s);
                return false;
            }
            if (aborted(s) || papp_time_us() > until) {
                snprintf(err, err_len, "secure connection to %.60s timed out", s->host);
                tp_close(s);
                return false;
            }
            papp_sleep_ms(10);
        }
    }
    if (papp_svc->net_tcp_connect == NULL || papp_svc->net_resolve == NULL) {
        snprintf(err, err_len, "this loader has no network services for apps");
        return false;
    }
    if (!parse_ipv4(s->host, &s->ip) && !papp_svc->net_resolve(s->host, &s->ip)) {
        snprintf(err, err_len, "could not find the server %.60s", s->host);
        return false;
    }
    s->h = papp_svc->net_tcp_connect(s->ip, s->port);
    if (s->h < 0) {
        snprintf(err, err_len, "could not connect to %.60s", s->host);
        return false;
    }
    const int64_t until = papp_time_us() + CONNECT_TIMEOUT_US;
    for (;;) {
        const int r = papp_svc->net_poll(s->h);
        if (r < 0 || (r & 4)) {
            snprintf(err, err_len, "%.60s refused the connection", s->host);
            tp_close(s);
            return false;
        }
        if (r & 2) {
            return true;
        }
        if (aborted(s) || papp_time_us() > until) {
            snprintf(err, err_len, "connecting to %.60s timed out", s->host);
            tp_close(s);
            return false;
        }
        papp_sleep_ms(10);
    }
}

// ── URLs ──────────────────────────────────────────────────────────────────

// http(s)://[user@]host[:port][/target] into the stream's host, port, tls
// and target.
static bool set_url(papp_stream_t *s, const char *url)
{
    const char *p;
    if (strncasecmp(url, "https://", 8) == 0) {
        s->tls = true;
        s->port = 443;
        p = url + 8;
    } else if (strncasecmp(url, "http://", 7) == 0) {
        s->tls = false;
        s->port = 80;
        p = url + 7;
    } else {
        return false;
    }
    const char *end = p + strcspn(p, "/?#");
    const char *at = memchr(p, '@', (size_t)(end - p));
    if (at != NULL) {
        p = at + 1;  // no user names or passwords
    }
    const char *colon = memchr(p, ':', (size_t)(end - p));
    const char *host_end = colon != NULL ? colon : end;
    if (host_end == p || host_end - p >= (int)sizeof(s->host)) {
        return false;
    }
    memcpy(s->host, p, (size_t)(host_end - p));
    s->host[host_end - p] = '\0';
    if (colon != NULL) {
        const long port = strtol(colon + 1, NULL, 10);
        if (port <= 0 || port > 65535) {
            return false;
        }
        s->port = (uint16_t)port;
    }
    // The target: the rest up to any fragment, spaces escaped.
    const char *t = *end == '/' || *end == '?' ? end : "/";
    size_t len = strcspn(t, "#");
    free(s->target);
    s->target = malloc(len * 3 + 2);
    if (s->target == NULL) {
        return false;
    }
    char *o = s->target;
    if (*t == '?') {
        *o++ = '/';
    }
    for (size_t i = 0; i < len; i++) {
        if (t[i] == ' ') {
            memcpy(o, "%20", 3);
            o += 3;
        } else {
            *o++ = t[i];
        }
    }
    *o = '\0';
    return true;
}

// A Location header against the current URL: absolute, //host/..., /path or
// relative to the current folder.
static char *resolve_location(papp_stream_t *s, const char *loc)
{
    const char *scheme = s->tls ? "https" : "http";
    char base[320];
    if ((s->tls && s->port == 443) || (!s->tls && s->port == 80)) {
        snprintf(base, sizeof(base), "%s://%s", scheme, s->host);
    } else {
        snprintf(base, sizeof(base), "%s://%s:%u", scheme, s->host, (unsigned)s->port);
    }
    size_t need = strlen(base) + strlen(s->target) + strlen(loc) + 8;
    char *out = malloc(need);
    if (out == NULL) {
        return NULL;
    }
    if (strncasecmp(loc, "http://", 7) == 0 || strncasecmp(loc, "https://", 8) == 0) {
        snprintf(out, need, "%s", loc);
    } else if (loc[0] == '/' && loc[1] == '/') {
        snprintf(out, need, "%s:%s", scheme, loc);
    } else if (loc[0] == '/') {
        snprintf(out, need, "%s%s", base, loc);
    } else {
        const char *slash = strrchr(s->target, '/');
        const size_t keep = slash != NULL ? (size_t)(slash - s->target) + 1 : 1;
        snprintf(out, need, "%s%.*s%s", base, (int)keep, slash != NULL ? s->target : "/", loc);
    }
    return out;
}

// ── HTTP responses ────────────────────────────────────────────────────────

static bool header_is(const char *line, const char *name, const char **value)
{
    const size_t n = strlen(name);
    if (strncasecmp(line, name, n) != 0 || line[n] != ':') {
        return false;
    }
    const char *v = line + n + 1;
    while (*v == ' ' || *v == '\t') {
        v++;
    }
    *value = v;
    return true;
}

// Sends a GET for bytes from `from` on and reads the answer's headers. Follows
// redirects. On success the connection is at the body's first byte, which
// is at offset conn_pos.
static bool http_request(papp_stream_t *s, int64_t from, char *err, size_t err_len)
{
    for (int hop = 0; hop <= MAX_REDIRECTS; hop++) {
        if (!tp_open(s, err, err_len)) {
            return false;
        }
        char host_hdr[300];
        if ((s->tls && s->port == 443) || (!s->tls && s->port == 80)) {
            snprintf(host_hdr, sizeof(host_hdr), "%s", s->host);
        } else {
            snprintf(host_hdr, sizeof(host_hdr), "%s:%u", s->host, (unsigned)s->port);
        }
        const size_t req_len = strlen(s->target) + strlen(host_hdr) + 256;
        char *req = malloc(req_len);
        if (req == NULL) {
            snprintf(err, err_len, "out of memory");
            return false;
        }
        const int n = snprintf(req, req_len,
                               "GET %s HTTP/1.1\r\nHost: %s\r\nUser-Agent: video/0.1 (ESP32-P4; Lavf)\r\n"
                               "Accept: */*\r\nAccept-Encoding: identity\r\nRange: bytes=%lld-\r\n"
                               "Connection: close\r\n\r\n",
                               s->target, host_hdr, (long long)from);
        const bool sent = tp_send_all(s, req, n);
        free(req);
        if (!sent) {
            snprintf(err, err_len, "lost the connection to %.60s", s->host);
            tp_close(s);
            return false;
        }

        // Headers, byte by byte from the receive buffer.
        static char line[HEADER_MAX];
        size_t line_len = 0;
        int status = 0, lines = 0;
        int64_t length = -1, range_start = -1, range_total = -1;
        bool chunked = false;
        char *location = NULL;
        s->raw_off = s->raw_len = 0;
        const int64_t until = papp_time_us() + IDLE_TIMEOUT_US;
        bool done = false;
        while (!done) {
            if (s->raw_off == s->raw_len) {
                const int got = tp_recv(s, s->raw, sizeof(s->raw));
                if (got == 0) {
                    if (aborted(s) || papp_time_us() > until) {
                        snprintf(err, err_len, "%.60s did not answer", s->host);
                        free(location);
                        tp_close(s);
                        return false;
                    }
                    papp_sleep_ms(10);
                    continue;
                }
                if (got < 0) {
                    snprintf(err, err_len, "%.60s closed the connection without an answer", s->host);
                    free(location);
                    tp_close(s);
                    return false;
                }
                s->raw_off = 0;
                s->raw_len = got;
            }
            const char c = (char)s->raw[s->raw_off++];
            if (c != '\n') {
                if (c != '\r' && line_len < sizeof(line) - 1) {
                    line[line_len++] = c;
                }
                continue;
            }
            line[line_len] = '\0';
            const char *v;
            if (lines++ == 0) {
                const char *sp = strchr(line, ' ');
                status = strncmp(line, "HTTP/", 5) == 0 && sp != NULL ? atoi(sp + 1) : 0;
                if (status == 0) {
                    snprintf(err, err_len, "%.60s did not answer with HTTP", s->host);
                    free(location);
                    tp_close(s);
                    return false;
                }
            } else if (line_len == 0) {
                if (status >= 100 && status < 200) {
                    lines = 0;  // an interim answer; the real one follows
                } else {
                    done = true;
                }
            } else if (header_is(line, "Content-Length", &v)) {
                length = strtoll(v, NULL, 10);
            } else if (header_is(line, "Content-Range", &v)) {
                // bytes a-b/total (total may be *)
                const char *d = strchr(v, ' ');
                if (d != NULL) {
                    range_start = strtoll(d + 1, NULL, 10);
                    const char *slash = strchr(d, '/');
                    if (slash != NULL && slash[1] != '*') {
                        range_total = strtoll(slash + 1, NULL, 10);
                    }
                }
            } else if (header_is(line, "Transfer-Encoding", &v)) {
                chunked = strcasestr(v, "chunked") != NULL;
            } else if (header_is(line, "Location", &v)) {
                free(location);
                location = strdup(v);
            }
            line_len = 0;
        }

        if (status >= 300 && status < 400 && location != NULL) {
            char *next = resolve_location(s, location);
            free(location);
            tp_close(s);
            if (next == NULL || !set_url(s, next)) {
                snprintf(err, err_len, "bad redirect");
                free(next);
                return false;
            }
            papp_log("redirected to %.180s", next);
            free(next);
            continue;
        }
        free(location);
        s->chunked = chunked;
        s->chunk_state = CH_SIZE;
        s->chunk_left = 0;
        s->conn_eof = false;
        if (status == 206) {
            s->conn_pos = range_start >= 0 ? range_start : from;
            s->seekable = true;
            if (range_total >= 0) {
                s->size = range_total;
            }
            s->body_left = chunked ? -1 : length;
            return true;
        }
        if (status == 200) {
            // The whole resource: the server ignored the range.
            s->conn_pos = 0;
            if (from > 0) {
                s->seekable = false;
            }
            if (!chunked && length >= 0) {
                s->size = length;
            }
            s->body_left = chunked ? -1 : length;
            return true;
        }
        if (status == 416) {
            s->conn_pos = from;
            s->conn_eof = true;
            s->body_left = 0;
            if (s->size < 0) {
                s->size = from;
            }
            return true;
        }
        snprintf(err, err_len, "%.60s answered HTTP %d", s->host, status);
        tp_close(s);
        return false;
    }
    snprintf(err, err_len, "too many redirects");
    return false;
}

// Body bytes after transfer decoding into dst: > 0 bytes, 0 none ready (only
// when !wait), -1 end of this response's body, -2 failed.
static int body_read(papp_stream_t *s, uint8_t *dst, int max, bool wait)
{
    const int64_t until = papp_time_us() + IDLE_TIMEOUT_US;
    while (max > 0) {
        if (s->conn_eof || s->h < 0) {
            return -1;
        }
        if (!s->chunked && s->body_left == 0) {
            s->conn_eof = true;
            return -1;
        }
        if (s->raw_off == s->raw_len) {
            const int n = tp_recv(s, s->raw, sizeof(s->raw));
            if (n == 0) {
                if (!wait) {
                    return 0;
                }
                if (aborted(s)) {
                    return -2;
                }
                if (papp_time_us() > until) {
                    papp_log("%s stopped sending", s->host);
                    return -2;
                }
                papp_sleep_ms(10);
                continue;
            }
            if (n < 0) {
                s->conn_eof = true;
                return n == -1 ? -1 : -2;  // a closed read-to-close body ends here
            }
            s->raw_off = 0;
            s->raw_len = n;
        }
        const int avail = s->raw_len - s->raw_off;
        if (!s->chunked) {
            int take = avail < max ? avail : max;
            if (s->body_left >= 0 && take > s->body_left) {
                take = (int)s->body_left;
            }
            memcpy(dst, s->raw + s->raw_off, (size_t)take);
            s->raw_off += take;
            s->conn_pos += take;
            if (s->body_left > 0) {
                s->body_left -= take;
            }
            return take;
        }
        if (s->chunk_state == CH_DATA) {
            int take = avail < max ? avail : max;
            if (take > s->chunk_left) {
                take = (int)s->chunk_left;
            }
            memcpy(dst, s->raw + s->raw_off, (size_t)take);
            s->raw_off += take;
            s->conn_pos += take;
            s->chunk_left -= take;
            if (s->chunk_left == 0) {
                s->chunk_state = CH_DATA_END;
            }
            return take;
        }
        // Chunk framing, one byte at a time.
        const char c = (char)s->raw[s->raw_off++];
        switch (s->chunk_state) {
        case CH_SIZE:
            if (isxdigit((unsigned char)c)) {
                const int d = isdigit((unsigned char)c) ? c - '0' : (tolower((unsigned char)c) - 'a' + 10);
                s->chunk_left = s->chunk_left * 16 + d;
            } else if (c == ';') {
                s->chunk_state = CH_EXT;
            } else if (c == '\n') {
                s->chunk_state = s->chunk_left > 0 ? CH_DATA : CH_TRAILER;
            }
            break;
        case CH_EXT:
            if (c == '\n') {
                s->chunk_state = s->chunk_left > 0 ? CH_DATA : CH_TRAILER;
            }
            break;
        case CH_DATA_END:
            if (c == '\n') {
                s->chunk_state = CH_SIZE;
                s->chunk_left = 0;
            }
            break;
        case CH_TRAILER:
            if (c == '\n') {
                s->conn_eof = true;  // the empty line after the last chunk
                return -1;
            }
            if (c != '\r') {
                s->chunk_state = CH_TRAILER_LINE;
            }
            break;
        case CH_TRAILER_LINE:
            if (c == '\n') {
                s->chunk_state = CH_TRAILER;
            }
            break;
        default:
            break;
        }
    }
    return 0;
}

// ── The ring ──────────────────────────────────────────────────────────────

// Receives into the ring at ring_end: up to `want` bytes, waiting for the
// first when `wait`. Returns bytes added, 0 none ready, -1 end, -2 failed.
static int ring_fill(papp_stream_t *s, int want, bool wait)
{
    int added = 0;
    while (added < want) {
        const size_t at = (size_t)(s->ring_end % RING_BYTES);
        int room = (int)(RING_BYTES - at);
        if (room > want - added) {
            room = want - added;
        }
        const int n = body_read(s, s->ring + at, room, wait && added == 0);
        if (n <= 0) {
            return added > 0 ? added : n;
        }
        s->ring_end += n;
        if (s->ring_end - s->ring_start > RING_BYTES) {
            s->ring_start = s->ring_end - RING_BYTES;
        }
        added += n;
    }
    return added;
}

static int http_read(papp_stream_t *s, uint8_t *buf, int size)
{
    if (s->size >= 0 && s->pos >= s->size) {
        return 0;
    }
    const int64_t skip = s->tls ? SKIP_TLS : SKIP_TCP;
    const bool in_ring = s->pos >= s->ring_start && s->pos < s->ring_end;
    const bool reachable = s->h >= 0 && !s->conn_eof && s->conn_pos == s->ring_end && s->pos >= s->ring_end &&
                           s->pos - s->ring_end <= skip;
    if (!in_ring && !reachable) {
        if (!s->seekable && s->pos < s->ring_start) {
            papp_log("cannot go back to %lld in a stream that is not seekable", (long long)s->pos);
            return -1;
        }
        if (s->seekable || s->h < 0 || s->conn_eof) {
            char err[160];
            if (!http_request(s, s->pos, err, sizeof(err))) {
                papp_log("%s", err);
                return -1;
            }
            s->ring_start = s->ring_end = s->conn_pos;
            if (s->conn_pos > s->pos) {
                return -1;
            }
        }
    }
    while (s->ring_end <= s->pos) {
        const int n = ring_fill(s, FILL_CHUNK, true);
        if (n == -1) {
            if (s->size < 0 || s->ring_end < s->size) {
                // The body ended early (or its length was never given).
                if (s->seekable && s->size > s->ring_end && !aborted(s)) {
                    char err[160];
                    if (http_request(s, s->ring_end, err, sizeof(err)) && s->conn_pos == s->ring_end) {
                        continue;
                    }
                }
                s->size = s->ring_end;
            }
            return 0;
        }
        if (n < 0) {
            return -1;
        }
        // Keep what is behind the read position only up to KEEP_BEHIND.
    }
    const int64_t have = s->ring_end - s->pos;
    int n = size < have ? size : (int)have;
    const size_t at = (size_t)(s->pos % RING_BYTES);
    const int first = (int)(RING_BYTES - at) < n ? (int)(RING_BYTES - at) : n;
    memcpy(buf, s->ring + at, (size_t)first);
    if (n > first) {
        memcpy(buf + first, s->ring, (size_t)(n - first));
    }
    s->pos += n;
    return n;
}

// ── Public API ────────────────────────────────────────────────────────────

static int hexval(char c)
{
    if (c >= '0' && c <= '9') {
        return c - '0';
    }
    c = (char)tolower((unsigned char)c);
    return c >= 'a' && c <= 'f' ? c - 'a' + 10 : -1;
}

papp_stream_t *papp_stream_open(const char *location, volatile int *abort_flag, char *err, size_t err_len)
{
    papp_stream_t *s = calloc(1, sizeof(*s));
    if (s == NULL) {
        snprintf(err, err_len, "out of memory");
        return NULL;
    }
    s->abort_flag = abort_flag;
    s->size = -1;
    s->h = -1;
    if (strncasecmp(location, "http://", 7) == 0 || strncasecmp(location, "https://", 8) == 0) {
        s->http = true;
        if (!set_url(s, location)) {
            snprintf(err, err_len, "not a URL this player understands");
            papp_stream_close(s);
            return NULL;
        }
        s->ring = papp_alloc_aligned(RING_BYTES, 64);
        if (s->ring == NULL) {
            snprintf(err, err_len, "no memory for the read-ahead buffer");
            papp_stream_close(s);
            return NULL;
        }
        if (!http_request(s, 0, err, err_len)) {
            papp_stream_close(s);
            return NULL;
        }
        s->ring_start = s->ring_end = s->conn_pos;
        papp_log("http: %s:%u%.100s, %s, size %lld", s->host, (unsigned)s->port, s->target,
                 s->seekable ? "seekable" : "not seekable", (long long)s->size);
        return s;
    }

    // A file: /sd/..., or a file:// URL of one (%xx escapes decoded).
    char path[512];
    const char *p = location;
    const bool file_url = strncasecmp(p, "file://", 7) == 0;
    if (file_url) {
        p += 7;
        if (strncasecmp(p, "localhost/", 10) == 0) {
            p += 9;
        }
    }
    size_t o = 0;
    for (; *p != '\0' && o < sizeof(path) - 1; p++) {
        if (file_url && *p == '%' && hexval(p[1]) >= 0 && hexval(p[2]) >= 0) {
            path[o++] = (char)(hexval(p[1]) * 16 + hexval(p[2]));
            p += 2;
        } else {
            path[o++] = *p;
        }
    }
    path[o] = '\0';
    s->fp = papp_svc->file_open(path, "rb");
    if (s->fp == NULL) {
        snprintf(err, err_len, "cannot open %.100s", path);
        papp_stream_close(s);
        return NULL;
    }
    if (papp_svc->file_seek(s->fp, 0, 2 /* SEEK_END */) == 0) {
        s->size = papp_svc->file_tell(s->fp);
    }
    papp_svc->file_seek(s->fp, 0, 0 /* SEEK_SET */);
    return s;
}

int papp_stream_read(papp_stream_t *s, uint8_t *buf, int size)
{
    if (aborted(s)) {
        return -1;
    }
    if (s->http) {
        return http_read(s, buf, size);
    }
    const size_t n = papp_svc->file_read(buf, 1, (size_t)size, s->fp);
    s->pos += (int64_t)n;
    return (int)n;
}

int papp_stream_seek(papp_stream_t *s, int64_t pos)
{
    if (pos < 0) {
        return -1;
    }
    if (!s->http) {
        if (pos > 0x7FFFFFFFLL || papp_svc->file_seek(s->fp, (long)pos, 0 /* SEEK_SET */) != 0) {
            return -1;
        }
        s->pos = pos;
        return 0;
    }
    if (!s->seekable && pos < s->ring_start) {
        return -1;
    }
    s->pos = pos;
    return 0;
}

int64_t papp_stream_size(papp_stream_t *s) { return s->size; }
int64_t papp_stream_tell(papp_stream_t *s) { return s->pos; }
bool papp_stream_seekable(papp_stream_t *s) { return !s->http || s->seekable; }

int64_t papp_stream_buffered(papp_stream_t *s)
{
    return s->http && s->ring_end > s->pos ? s->ring_end - s->pos : 0;
}

void papp_stream_prefetch(papp_stream_t *s)
{
    if (!s->http || s->h < 0 || s->conn_eof || s->conn_pos != s->ring_end || s->pos < s->ring_start ||
        s->pos > s->ring_end) {
        return;
    }
    // Room ahead of the read position, keeping KEEP_BEHIND bytes behind it.
    int64_t room = (int64_t)RING_BYTES - KEEP_BEHIND - (s->ring_end - s->pos);
    if (room <= 0) {
        return;
    }
    if (room > 256 * 1024) {
        room = 256 * 1024;
    }
    ring_fill(s, (int)room, false);
}

void papp_stream_close(papp_stream_t *s)
{
    if (s == NULL) {
        return;
    }
    tp_close(s);
    if (s->fp != NULL) {
        papp_svc->file_close(s->fp);
    }
    free(s->ring);
    free(s->target);
    free(s);
}

void papp_stream_shutdown(void)
{
    // Streams are closed by the player; the loader closes any socket or TLS
    // session an app leaves open.
}
