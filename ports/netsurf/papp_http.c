// HTTP and HTTPS fetcher for the NetSurf PAPP, on the loader's net_* and
// net_tls_* services instead of libcurl. Registered for http: and https:
// (patches/0002 calls papp_fetch_http_register() from fetcher_init);
// NetSurf's own file:, about:, resource: and data: fetchers stay as they are.
//
// HTTP/1.1: GET and POST (URL-encoded and multipart), redirects (NetSurf
// follows them, between http: and https: too), chunked transfer coding,
// Content-Length or read-to-close bodies, gzip content coding (zlib),
// cookies, Basic authentication, If-Modified-Since / If-None-Match from
// NetSurf's cache, and an optional HTTP proxy for http: (the http_proxy
// options). Connections are kept alive and reused for the next request to
// the same server (one TLS handshake per server, not per image); a request
// on a kept connection that turns out to be closed is sent again on a new
// one (never a POST).
//
// Nothing blocks: NetSurf's scheduler polls fetchers every 10 ms while any is
// active, and each poll advances every fetch a step. Host names are looked
// up by a small task (net_resolve blocks) and cached.
//
// The byte stream goes through a transport: plain TCP (net_tcp_*), or TLS
// (net_tls_*), where the loader does the TCP connect and the handshake on its
// own task and verifies the certificate against the firmware's CA bundle
// with SNI; there is no way to skip that check, so a failure shows NetSurf's
// certificate error page without its "proceed" button (patches/0008). The
// loader has at most 4 TLS sessions for all apps; this fetcher uses 3.
#include "papp_port.h"

#include <ctype.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <zlib.h>

#include <libwapcaplet/libwapcaplet.h>
#include <nsutils/base64.h>

#include "utils/corestrings.h"
#include "utils/errors.h"
#include "utils/log.h"
#include "utils/messages.h"
#include "utils/nsoption.h"
#include "utils/nsurl.h"
#include "utils/time.h"
#include "utils/useragent.h"
#include "content/fetch.h"
#include "content/fetchers.h"
#include "content/urldb.h"

nserror papp_fetch_http_register(void);
int papp_have_tls(void);

#define RECV_BUF 4096
#define MAX_READ_PER_POLL (48 * 1024)       // bytes one fetch may take per poll
#define MAX_LINE 8192                        // longest header line kept
#define MAX_UPLOAD (8 * 1024 * 1024)         // largest file a form may post
#define CONNECT_RETRY_US 4000000             // how long to retry when the loader refuses a connection
#define WAIT_FOR_SESSION_US 120000000        // how long to queue for a TLS session
#define TLS_CONNECT_TIMEOUT_US 60000000      // lookup + TCP + handshake, on the loader's task
#define IDLE_TIMEOUT_US 60000000             // no data for this long: timed out
#define KEEP_IDLE_US 15000000                // a kept connection is closed after this long unused
#define TLS_MAX 3                            // TLS handles this app holds at once (the loader has 4)

enum state { ST_RESOLVE, ST_CONNECT, ST_CONNECTING, ST_SEND, ST_STATUS, ST_HEADERS, ST_BODY };
enum body_mode { BODY_NONE, BODY_LENGTH, BODY_CHUNKED, BODY_TO_CLOSE };
enum chunk_state { CH_SIZE, CH_DATA, CH_DATA_END, CH_TRAILER };

// A byte stream to a server.
struct transport {
    bool tls;
    int (*open)(const char *host, uint32_t ip, uint16_t port);  // handle, -1 refused (try again), -2 busy
    int (*ready)(int h);                                         // 1 connected, 0 not yet, -1 failed
    int (*send)(int h, const void *buf, int len);                // bytes, 0 would block, -1 failed
    int (*recv)(int h, void *buf, int len);                      // bytes, 0 closed, -2 nothing yet, -1 failed
    void (*close)(int h);
};

struct http_ctx {
    struct http_ctx *next;
    struct fetch *fetch;
    nsurl *url;
    const struct transport *tp;
    bool https;
    bool proxy;
    bool only_2xx;
    bool post;
    bool started, aborted, dead, locked;
    enum state state;
    char host[256];  // where to connect: the server, or the proxy
    uint16_t port;
    uint32_t ip;
    int dns;         // lookup slot, or -1
    int sock;        // transport handle, or -1
    bool reused;     // sock came from the keep-alive pool
    char *req;
    size_t req_len, req_sent;
    int64_t deadline;
    int64_t retry_until;
    int64_t queued_until;
    bool got_bytes;
    // response
    char line[MAX_LINE + 1];
    size_t line_len;
    long http_code;
    bool http10;
    bool conn_close;
    bool trailing;   // bytes after the end of the body: the connection is not reusable
    long long content_length;
    bool chunked;
    bool gzip;
    bool zinit;
    z_stream z;
    enum body_mode body;
    long long remaining;
    enum chunk_state chunk;
    char *location;
    char *realm;
    char message[200];  // error/progress text handed to NetSurf
};

static struct http_ctx *s_list = NULL;
static bool s_polling = false;

// ── Host name lookups ───────────────────────────────────────────────────────

#define DNS_JOBS 6
#define DNS_CACHE 16
#define DNS_TTL_US (300LL * 1000000)

enum { DNS_FREE, DNS_QUEUED, DNS_DONE };

struct dns_job {
    volatile int state;
    volatile int abandoned;
    char host[256];
    uint32_t ip;
    int ok;
};

static struct dns_job s_jobs[DNS_JOBS];
static void *s_dns_task = NULL;
static volatile int s_dns_stop = 0;
static volatile int s_dns_running = 0;

static struct {
    char host[64];
    uint32_t ip;
    int64_t expires;
} s_dns_cache[DNS_CACHE];

static void dns_worker(void *arg)
{
    (void)arg;
    s_dns_running = 1;
    while (!s_dns_stop) {
        bool busy = false;
        for (int i = 0; i < DNS_JOBS && !s_dns_stop; i++) {
            struct dns_job *j = &s_jobs[i];
            if (__atomic_load_n(&j->state, __ATOMIC_ACQUIRE) == DNS_QUEUED) {
                uint32_t ip = 0;
                j->ok = papp_svc->net_resolve(j->host, &ip);
                j->ip = ip;
                __atomic_store_n(&j->state, DNS_DONE, __ATOMIC_RELEASE);
                busy = true;
            }
        }
        if (!busy) {
            papp_svc->delay_ms(10);
        }
    }
    s_dns_running = 0;
    for (;;) {
        papp_svc->delay_ms(1000);  // deleted by papp_http_shutdown
    }
}

static bool parse_ipv4(const char *s, uint32_t *ip)
{
    unsigned a[4];
    char tail;
    if (sscanf(s, "%3u.%3u.%3u.%3u%c", &a[0], &a[1], &a[2], &a[3], &tail) != 4) {
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

static bool dns_cached(const char *host, uint32_t *ip)
{
    const int64_t now = papp_time_us();
    for (int i = 0; i < DNS_CACHE; i++) {
        if (s_dns_cache[i].host[0] != '\0' && s_dns_cache[i].expires > now &&
            strcasecmp(s_dns_cache[i].host, host) == 0) {
            *ip = s_dns_cache[i].ip;
            return true;
        }
    }
    return false;
}

static void dns_remember(const char *host, uint32_t ip)
{
    if (strlen(host) >= sizeof(s_dns_cache[0].host)) {
        return;
    }
    int slot = 0;
    for (int i = 0; i < DNS_CACHE; i++) {
        if (s_dns_cache[i].expires < s_dns_cache[slot].expires) {
            slot = i;
        }
    }
    strcpy(s_dns_cache[slot].host, host);
    s_dns_cache[slot].ip = ip;
    s_dns_cache[slot].expires = papp_time_us() + DNS_TTL_US;
}

// Finished lookups nobody waits for any more.
static void dns_reap(void)
{
    for (int i = 0; i < DNS_JOBS; i++) {
        struct dns_job *j = &s_jobs[i];
        if (j->abandoned && __atomic_load_n(&j->state, __ATOMIC_ACQUIRE) == DNS_DONE) {
            j->abandoned = 0;
            __atomic_store_n(&j->state, DNS_FREE, __ATOMIC_RELEASE);
        }
    }
}

// A lookup slot for host, or -1 when all are busy (try again next poll).
static int dns_start(const char *host)
{
    if (s_dns_task == NULL) {
        s_dns_stop = 0;
        if (papp_svc->task_create(dns_worker, "ns_dns", 6144, NULL, 3, &s_dns_task, -1) != 0) {
            s_dns_task = NULL;
            return -1;
        }
    }
    dns_reap();
    for (int i = 0; i < DNS_JOBS; i++) {
        struct dns_job *j = &s_jobs[i];
        if (__atomic_load_n(&j->state, __ATOMIC_ACQUIRE) == DNS_FREE && !j->abandoned) {
            snprintf(j->host, sizeof(j->host), "%s", host);
            j->ok = 0;
            __atomic_store_n(&j->state, DNS_QUEUED, __ATOMIC_RELEASE);
            return i;
        }
    }
    return -1;
}

// 1 resolved, 0 still looking, -1 unknown host.
static int dns_result(int slot, uint32_t *ip)
{
    struct dns_job *j = &s_jobs[slot];
    if (__atomic_load_n(&j->state, __ATOMIC_ACQUIRE) != DNS_DONE) {
        return 0;
    }
    const int ok = j->ok;
    *ip = j->ip;
    __atomic_store_n(&j->state, DNS_FREE, __ATOMIC_RELEASE);
    return ok ? 1 : -1;
}

static void dns_abandon(int slot)
{
    if (slot >= 0) {
        s_jobs[slot].abandoned = 1;
        dns_reap();
    }
}

// ── Transports ──────────────────────────────────────────────────────────────

static int s_tls_open = 0;  // TLS handles held: fetches and kept connections

static int tcp_open(const char *host, uint32_t ip, uint16_t port)
{
    (void)host;
    const int h = papp_svc->net_tcp_connect(ip, port);
    return h >= 0 ? h : -1;
}

static int tcp_ready(int h)
{
    const int r = papp_svc->net_poll(h);
    if (r < 0 || (r & 4)) {
        return -1;
    }
    return (r & 2) ? 1 : 0;
}

static int tcp_send(int h, const void *buf, int len)
{
    return papp_svc->net_tcp_send(h, buf, len);
}

static int tcp_recv(int h, void *buf, int len)
{
    return papp_svc->net_tcp_recv(h, buf, len);
}

static void tcp_close(int h)
{
    papp_svc->net_udp_close(h);  // closes TCP handles too
}

static const struct transport tcp_transport = {false, tcp_open, tcp_ready, tcp_send, tcp_recv, tcp_close};

static bool drop_kept_tls(void);

static int tls_open(const char *host, uint32_t ip, uint16_t port)
{
    (void)ip;  // the loader looks the name up itself, for SNI and the certificate
    if (s_tls_open >= TLS_MAX && !drop_kept_tls()) {
        return -2;
    }
    const int h = papp_svc->net_tls_connect(host, port);
    if (h < 0) {
        return -1;
    }
    s_tls_open++;
    return h;
}

static int tls_ready(int h)
{
    const int s = papp_svc->net_tls_status(h);
    return s > 0 ? 1 : (s == 0 ? 0 : -1);
}

static int tls_send(int h, const void *buf, int len)
{
    return papp_svc->net_tls_send(h, buf, len);  // 0: send the same bytes again later
}

static int tls_recv(int h, void *buf, int len)
{
    const int n = papp_svc->net_tls_recv(h, buf, len);
    if (n > 0) {
        return n;
    }
    if (n == 0) {
        return -2;
    }
    return papp_svc->net_tls_status(h) == 1 ? 0 : -1;  // a clean close, or an error
}

static void tls_close(int h)
{
    papp_svc->net_tls_close(h);
    if (s_tls_open > 0) {
        s_tls_open--;
    }
}

static const struct transport tls_transport = {true, tls_open, tls_ready, tls_send, tls_recv, tls_close};

int papp_have_tls(void)
{
    return papp_svc != NULL && papp_svc->net_tls_connect != NULL && papp_svc->net_tls_status != NULL &&
           papp_svc->net_tls_send != NULL && papp_svc->net_tls_recv != NULL && papp_svc->net_tls_close != NULL;
}

// ── Kept-alive connections ──────────────────────────────────────────────────

#define KEEP_MAX 4

static struct {
    const struct transport *tp;  // NULL: slot free
    int h;
    char host[256];
    uint16_t port;
    int64_t since;
} s_keep[KEEP_MAX];

static void keep_close(int i)
{
    s_keep[i].tp->close(s_keep[i].h);
    s_keep[i].tp = NULL;
}

// Close the oldest kept TLS connection, so its session can serve a new one.
static bool drop_kept_tls(void)
{
    int oldest = -1;
    for (int i = 0; i < KEEP_MAX; i++) {
        if (s_keep[i].tp != NULL && s_keep[i].tp->tls && (oldest < 0 || s_keep[i].since < s_keep[oldest].since)) {
            oldest = i;
        }
    }
    if (oldest < 0) {
        return false;
    }
    keep_close(oldest);
    return true;
}

static void keep_put(const struct transport *tp, int h, const char *host, uint16_t port)
{
    int slot = -1;
    for (int i = 0; i < KEEP_MAX; i++) {
        if (s_keep[i].tp == NULL) {
            slot = i;
            break;
        }
        if (slot < 0 || s_keep[i].since < s_keep[slot].since) {
            slot = i;
        }
    }
    if (s_keep[slot].tp != NULL) {
        keep_close(slot);
    }
    s_keep[slot].tp = tp;
    s_keep[slot].h = h;
    snprintf(s_keep[slot].host, sizeof(s_keep[slot].host), "%s", host);
    s_keep[slot].port = port;
    s_keep[slot].since = papp_time_us();
}

static int keep_take(const struct transport *tp, const char *host, uint16_t port)
{
    for (int i = 0; i < KEEP_MAX; i++) {
        if (s_keep[i].tp == tp && s_keep[i].port == port && strcasecmp(s_keep[i].host, host) == 0) {
            s_keep[i].tp = NULL;
            return s_keep[i].h;
        }
    }
    return -1;
}

// Kept connections that timed out, or that the server closed (anything to
// read on an idle connection means that), are closed.
static void keep_check(int64_t now)
{
    for (int i = 0; i < KEEP_MAX; i++) {
        if (s_keep[i].tp == NULL) {
            continue;
        }
        const int r = papp_svc->net_poll(s_keep[i].h);
        if (now - s_keep[i].since > KEEP_IDLE_US || r < 0 || (r & 5) != 0) {
            keep_close(i);
        }
    }
}

void papp_http_shutdown(void)
{
    for (int i = 0; i < KEEP_MAX; i++) {
        if (s_keep[i].tp != NULL) {
            keep_close(i);
        }
    }
    if (s_dns_task == NULL) {
        return;
    }
    s_dns_stop = 1;
    // A lookup in progress is left to finish (lwIP holds on to its task).
    for (int i = 0; i < 1000 && s_dns_running; i++) {
        papp_svc->delay_ms(10);
    }
    papp_svc->task_delete(s_dns_task);
    s_dns_task = NULL;
}

static void close_conn(struct http_ctx *c)
{
    if (c->sock >= 0) {
        c->tp->close(c->sock);
        c->sock = -1;
    }
}

// ── Callbacks into NetSurf ──────────────────────────────────────────────────

// Returns false when NetSurf aborted the fetch from inside the callback.
static bool send_msg(struct http_ctx *c, fetch_msg *msg)
{
    c->locked = true;
    fetch_send_callback(msg, c->fetch);
    c->locked = false;
    if (c->aborted) {
        c->dead = true;
        return false;
    }
    return true;
}

// The fetch's last message; nothing more is sent after it.
static void finish_with(struct http_ctx *c, fetch_msg_type type, const char *text)
{
    if (c->dead) {
        return;
    }
    fetch_msg msg;
    memset(&msg, 0, sizeof(msg));
    msg.type = type;
    if (type == FETCH_ERROR || type == FETCH_TIMEDOUT) {
        msg.data.error = text;
    } else if (type == FETCH_REDIRECT) {
        msg.data.redirect = text;
    } else if (type == FETCH_AUTH) {
        msg.data.auth.realm = text;
    }
    close_conn(c);
    send_msg(c, &msg);
    c->dead = true;
}

static void fail(struct http_ctx *c, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(c->message, sizeof(c->message), fmt, ap);
    va_end(ap);
    NSLOG(fetch, INFO, "%s: %s", nsurl_access(c->url), c->message);
    finish_with(c, FETCH_ERROR, c->message);
}

static void timed_out(struct http_ctx *c, const char *fmt)
{
    snprintf(c->message, sizeof(c->message), fmt, c->host);
    finish_with(c, FETCH_TIMEDOUT, c->message);
}

static void progress(struct http_ctx *c, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(c->message, sizeof(c->message), fmt, ap);
    va_end(ap);
    fetch_msg msg;
    memset(&msg, 0, sizeof(msg));
    msg.type = FETCH_PROGRESS;
    msg.data.progress = c->message;
    send_msg(c, &msg);
}

static bool send_data(struct http_ctx *c, const uint8_t *data, size_t len)
{
    fetch_msg msg;
    memset(&msg, 0, sizeof(msg));
    msg.type = FETCH_DATA;
    msg.data.header_or_data.buf = data;
    msg.data.header_or_data.len = len;
    return send_msg(c, &msg);
}

// ── Request ─────────────────────────────────────────────────────────────────

struct buf {
    char *p;
    size_t len, cap;
    bool failed;
};

static void buf_add(struct buf *b, const void *data, size_t len)
{
    if (b->failed) {
        return;
    }
    if (b->len + len + 1 > b->cap) {
        size_t cap = b->cap ? b->cap : 1024;
        while (cap < b->len + len + 1) {
            cap *= 2;
        }
        char *p = realloc(b->p, cap);
        if (p == NULL) {
            b->failed = true;
            return;
        }
        b->p = p;
        b->cap = cap;
    }
    memcpy(b->p + b->len, data, len);
    b->len += len;
    b->p[b->len] = '\0';
}

static void buf_printf(struct buf *b, const char *fmt, ...)
{
    char tmp[512];
    va_list ap;
    va_start(ap, fmt);
    const int n = vsnprintf(tmp, sizeof(tmp), fmt, ap);
    va_end(ap);
    if (n < 0) {
        return;
    }
    if ((size_t)n < sizeof(tmp)) {
        buf_add(b, tmp, (size_t)n);
        return;
    }
    char *big = malloc((size_t)n + 1);
    if (big == NULL) {
        b->failed = true;
        return;
    }
    va_start(ap, fmt);
    vsnprintf(big, (size_t)n + 1, fmt, ap);
    va_end(ap);
    buf_add(b, big, (size_t)n);
    free(big);
}

static void buf_basic_auth(struct buf *b, const char *header, const char *userpass)
{
    uint8_t *enc = NULL;
    size_t enc_len = 0;
    if (nsu_base64_encode_alloc((const uint8_t *)userpass, strlen(userpass), &enc, &enc_len) == NSUERROR_OK) {
        buf_printf(b, "%s: Basic ", header);
        buf_add(b, enc, enc_len);
        buf_add(b, "\r\n", 2);
        free(enc);
    }
}

static void add_file(struct buf *b, const char *path)
{
    FILE *f = (path != NULL && *path != '\0') ? fopen(path, "rb") : NULL;
    if (f == NULL) {
        return;
    }
    char chunk[2048];
    size_t n, total = 0;
    while ((n = fread(chunk, 1, sizeof(chunk), f)) > 0 && total < MAX_UPLOAD) {
        buf_add(b, chunk, n);
        total += n;
    }
    fclose(f);
}

static char *multipart_body(const struct fetch_multipart_data *mp, const char *boundary, size_t *len)
{
    struct buf b = {0};
    for (; mp != NULL; mp = mp->next) {
        buf_printf(&b, "--%s\r\n", boundary);
        if (mp->file) {
            buf_printf(&b, "Content-Disposition: form-data; name=\"%s\"; filename=\"%s\"\r\n"
                           "Content-Type: application/octet-stream\r\n\r\n", mp->name, mp->value);
            add_file(&b, mp->rawfile);
        } else {
            buf_printf(&b, "Content-Disposition: form-data; name=\"%s\"\r\n\r\n", mp->name);
            buf_add(&b, mp->value, strlen(mp->value));
        }
        buf_add(&b, "\r\n", 2);
    }
    buf_printf(&b, "--%s--\r\n", boundary);
    if (b.failed) {
        free(b.p);
        return NULL;
    }
    *len = b.len;
    return b.p;
}

static bool use_proxy(void)
{
    return nsoption_bool(http_proxy) && nsoption_charp(http_proxy_host) != NULL &&
           nsoption_charp(http_proxy_host)[0] != '\0';
}

static bool build_request(struct http_ctx *c, const char *post_urlenc, const struct fetch_multipart_data *post_multipart,
                          const char **headers)
{
    struct buf b = {0};
    char *target = NULL;
    size_t target_len = 0;
    c->proxy = use_proxy() && !c->https;
    nserror err = nsurl_get(c->url, c->proxy ? (NSURL_SCHEME | NSURL_HOST | NSURL_PORT | NSURL_PATH | NSURL_QUERY)
                                             : (NSURL_PATH | NSURL_QUERY),
                            &target, &target_len);
    if (err != NSERROR_OK) {
        return false;
    }
    c->post = post_urlenc != NULL || post_multipart != NULL;
    buf_printf(&b, "%s %s HTTP/1.1\r\n", c->post ? "POST" : "GET", target_len ? target : "/");
    free(target);

    lwc_string *host = nsurl_get_component(c->url, NSURL_HOST);
    lwc_string *port = nsurl_get_component(c->url, NSURL_PORT);
    if (host == NULL) {
        if (port) {
            lwc_string_unref(port);
        }
        free(b.p);
        return false;
    }
    if (port != NULL) {
        buf_printf(&b, "Host: %s:%s\r\n", lwc_string_data(host), lwc_string_data(port));
    } else {
        buf_printf(&b, "Host: %s\r\n", lwc_string_data(host));
    }
    if (c->proxy) {
        snprintf(c->host, sizeof(c->host), "%s", nsoption_charp(http_proxy_host));
        c->port = (uint16_t)nsoption_int(http_proxy_port);
    } else {
        snprintf(c->host, sizeof(c->host), "%s", lwc_string_data(host));
        c->port = port != NULL ? (uint16_t)atoi(lwc_string_data(port)) : (c->https ? 443 : 80);
    }
    lwc_string_unref(host);
    if (port) {
        lwc_string_unref(port);
    }

    buf_printf(&b, "User-Agent: %s\r\n", user_agent_string());
    // Only gzip: some servers send "deflate" without the zlib header.
    buf_printf(&b, "Accept: */*\r\nAccept-Encoding: gzip\r\n");
    if (nsoption_charp(accept_language) != NULL && nsoption_charp(accept_language)[0] != '\0') {
        buf_printf(&b, "Accept-Language: %s, *;q=0.1\r\n", nsoption_charp(accept_language));
    }
    if (nsoption_charp(accept_charset) != NULL && nsoption_charp(accept_charset)[0] != '\0') {
        buf_printf(&b, "Accept-Charset: %s, *;q=0.1\r\n", nsoption_charp(accept_charset));
    }
    if (nsoption_bool(do_not_track)) {
        buf_printf(&b, "DNT: 1\r\n");
    }

    char *cookie = urldb_get_cookie(c->url, true);
    if (cookie != NULL) {
        buf_printf(&b, "Cookie: %s\r\n", cookie);
        free(cookie);
    }
    const char *auth = urldb_get_auth_details(c->url, NULL);
    if (auth != NULL) {
        buf_basic_auth(&b, "Authorization", auth);
    }
    if (c->proxy && nsoption_int(http_proxy_auth) == OPTION_HTTP_PROXY_AUTH_BASIC) {
        char userpass[256];
        snprintf(userpass, sizeof(userpass), "%s:%s",
                 nsoption_charp(http_proxy_auth_user) ? nsoption_charp(http_proxy_auth_user) : "",
                 nsoption_charp(http_proxy_auth_pass) ? nsoption_charp(http_proxy_auth_pass) : "");
        buf_basic_auth(&b, "Proxy-Authorization", userpass);
    }
    for (int i = 0; headers != NULL && headers[i] != NULL; i++) {
        buf_printf(&b, "%s\r\n", headers[i]);
    }

    if (post_urlenc != NULL) {
        const size_t len = strlen(post_urlenc);
        buf_printf(&b, "Content-Type: application/x-www-form-urlencoded\r\nContent-Length: %u\r\n\r\n",
                   (unsigned)len);
        buf_add(&b, post_urlenc, len);
    } else if (post_multipart != NULL) {
        char boundary[48];
        snprintf(boundary, sizeof(boundary), "----NetSurfPAPP%08lx%08lx", (unsigned long)papp_time_us(),
                 (unsigned long)(uintptr_t)c);
        size_t body_len = 0;
        char *body = multipart_body(post_multipart, boundary, &body_len);
        if (body == NULL) {
            free(b.p);
            return false;
        }
        buf_printf(&b, "Content-Type: multipart/form-data; boundary=%s\r\nContent-Length: %u\r\n\r\n", boundary,
                   (unsigned)body_len);
        buf_add(&b, body, body_len);
        free(body);
    } else {
        buf_add(&b, "\r\n", 2);
    }
    if (b.failed) {
        free(b.p);
        return false;
    }
    c->req = b.p;
    c->req_len = b.len;
    return true;
}

// ── Response ────────────────────────────────────────────────────────────────

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

// The body is complete: keep the connection for the next request when the
// server allows it and nothing is left over on it, then tell NetSurf.
static void body_done(struct http_ctx *c)
{
    if (c->dead) {
        return;
    }
    if (c->sock >= 0 && !c->conn_close && !c->trailing && c->body != BODY_TO_CLOSE && !c->proxy) {
        keep_put(c->tp, c->sock, c->host, c->port);
        c->sock = -1;
    }
    finish_with(c, FETCH_FINISHED, NULL);
}

// The blank line after the headers: redirects, 304, 401 and errors end the
// fetch here (as NetSurf's curl fetcher does); otherwise the body follows.
static void headers_done(struct http_ctx *c)
{
    const long code = c->http_code;
    if (code == 304 && !c->post) {
        c->body = BODY_NONE;
        if (!c->conn_close && !c->trailing && c->sock >= 0 && !c->proxy) {
            keep_put(c->tp, c->sock, c->host, c->port);
            c->sock = -1;
        }
        finish_with(c, FETCH_NOTMODIFIED, NULL);
        return;
    }
    if (code >= 300 && code < 400 && c->location != NULL) {
        NSLOG(fetch, INFO, "redirect to %s", c->location);
        finish_with(c, FETCH_REDIRECT, c->location);
        return;
    }
    if (code == 401) {
        finish_with(c, FETCH_AUTH, c->realm);
        return;
    }
    if (c->only_2xx && (code < 200 || code > 299)) {
        finish_with(c, FETCH_ERROR, messages_get("Not2xx"));
        return;
    }
    if (code == 204 || code == 304) {
        c->body = BODY_NONE;
    } else if (c->chunked) {
        c->body = BODY_CHUNKED;
        c->chunk = CH_SIZE;
    } else if (c->content_length >= 0) {
        c->body = BODY_LENGTH;
        c->remaining = c->content_length;
    } else {
        c->body = BODY_TO_CLOSE;
    }
    if (c->gzip && c->body != BODY_NONE) {
        memset(&c->z, 0, sizeof(c->z));
        c->zinit = inflateInit2(&c->z, 15 + 32) == Z_OK;  // zlib or gzip header
        if (!c->zinit) {
            fail(c, "Could not start decompressing the page");
            return;
        }
    }
    c->state = ST_BODY;
    if (c->body == BODY_NONE || (c->body == BODY_LENGTH && c->remaining == 0)) {
        body_done(c);
    }
}

static void header_line(struct http_ctx *c, char *line, size_t len)
{
    if (c->state == ST_STATUS) {
        if (strncmp(line, "HTTP/", 5) != 0) {
            fail(c, "%s did not answer with HTTP", c->host);
            return;
        }
        const char *sp = strchr(line, ' ');
        c->http_code = sp ? strtol(sp + 1, NULL, 10) : 0;
        if (c->http_code < 100 || c->http_code > 999) {
            fail(c, "%s sent a bad status line", c->host);
            return;
        }
        c->http10 = strncmp(line, "HTTP/1.0", 8) == 0;
        c->conn_close = c->http10;  // HTTP/1.0 closes unless it says keep-alive
        fetch_set_http_code(c->fetch, (http_response_code)c->http_code);
        c->state = ST_HEADERS;
        return;
    }
    if (len == 0) {
        if (c->http_code >= 100 && c->http_code < 200) {
            c->state = ST_STATUS;  // an interim response; the real one follows
            return;
        }
        headers_done(c);
        return;
    }
    if (line[0] == ' ' || line[0] == '\t') {
        return;  // obsolete line folding
    }
    while (len > 0 && (line[len - 1] == ' ' || line[len - 1] == '\t')) {
        line[--len] = '\0';
    }

    fetch_msg msg;
    memset(&msg, 0, sizeof(msg));
    msg.type = FETCH_HEADER;
    msg.data.header_or_data.buf = (const uint8_t *)line;
    msg.data.header_or_data.len = len;
    if (!send_msg(c, &msg)) {
        return;
    }

    const char *v;
    if (header_is(line, "Location", &v)) {
        free(c->location);
        c->location = strdup(v);
    } else if (header_is(line, "Content-Length", &v)) {
        c->content_length = strtoll(v, NULL, 10);
        if (c->content_length < 0) {
            c->content_length = -1;
        }
    } else if (header_is(line, "Transfer-Encoding", &v)) {
        c->chunked = strcasestr(v, "chunked") != NULL;
    } else if (header_is(line, "Content-Encoding", &v)) {
        c->gzip = strcasecmp(v, "gzip") == 0 || strcasecmp(v, "x-gzip") == 0 || strcasecmp(v, "deflate") == 0;
    } else if (header_is(line, "Connection", &v)) {
        if (strcasestr(v, "close") != NULL) {
            c->conn_close = true;
        } else if (strcasestr(v, "keep-alive") != NULL) {
            c->conn_close = false;
        }
    } else if (header_is(line, "Set-Cookie", &v)) {
        fetch_set_cookie(c->fetch, v);
    } else if (header_is(line, "Date", &v)) {
        time_t t;
        if (nsc_strntimet(v, strlen(v), &t) == NSERROR_OK) {
            papp_set_wallclock(t);
        }
    } else if (header_is(line, "WWW-Authenticate", &v)) {
        const char *r = strcasestr(v, "realm=\"");
        if (r != NULL) {
            r += 7;
            const char *end = strchr(r, '"');
            if (end != NULL) {
                free(c->realm);
                c->realm = strndup(r, (size_t)(end - r));
            }
        }
    }
}

// Body bytes after transfer decoding: through zlib if compressed, then to NetSurf.
static bool deliver(struct http_ctx *c, const uint8_t *data, size_t len)
{
    if (len == 0) {
        return true;
    }
    if (!c->zinit) {
        return send_data(c, data, len);
    }
    static uint8_t out[8192];
    c->z.next_in = (Bytef *)data;
    c->z.avail_in = (uInt)len;
    while (c->z.avail_in > 0) {
        c->z.next_out = out;
        c->z.avail_out = sizeof(out);
        const int r = inflate(&c->z, Z_NO_FLUSH);
        const size_t n = sizeof(out) - c->z.avail_out;
        if (n > 0 && !send_data(c, out, n)) {
            return false;
        }
        if (r == Z_STREAM_END) {
            break;  // anything after the stream is ignored
        }
        if (r != Z_OK && r != Z_BUF_ERROR) {
            fail(c, "The page could not be decompressed");
            return false;
        }
        if (r == Z_BUF_ERROR && n == 0) {
            break;
        }
    }
    return true;
}

// Transfer decoding. Returns false once the fetch is over or aborted.
static bool body_bytes(struct http_ctx *c, const uint8_t *p, size_t n)
{
    switch (c->body) {
    case BODY_TO_CLOSE:
        return deliver(c, p, n);
    case BODY_LENGTH: {
        const size_t take = (long long)n < c->remaining ? n : (size_t)c->remaining;
        if (!deliver(c, p, take)) {
            return false;
        }
        c->remaining -= (long long)take;
        if (c->remaining == 0) {
            c->trailing = n > take;
            body_done(c);
            return false;
        }
        return true;
    }
    case BODY_CHUNKED:
        while (n > 0) {
            if (c->chunk == CH_DATA) {
                const size_t take = (long long)n < c->remaining ? n : (size_t)c->remaining;
                if (!deliver(c, p, take)) {
                    return false;
                }
                p += take;
                n -= take;
                c->remaining -= (long long)take;
                if (c->remaining == 0) {
                    c->chunk = CH_DATA_END;
                }
                continue;
            }
            const char ch = (char)*p++;
            n--;
            if (ch != '\n') {
                if (ch != '\r' && c->line_len < MAX_LINE) {
                    c->line[c->line_len++] = ch;
                }
                continue;
            }
            c->line[c->line_len] = '\0';
            const size_t len = c->line_len;
            c->line_len = 0;
            if (c->chunk == CH_SIZE) {
                char *end = NULL;
                const unsigned long size = strtoul(c->line, &end, 16);
                if (end == c->line) {
                    fail(c, "%s sent a bad chunk", c->host);
                    return false;
                }
                if (size == 0) {
                    c->chunk = CH_TRAILER;
                } else {
                    c->remaining = (long long)size;
                    c->chunk = CH_DATA;
                }
            } else if (c->chunk == CH_DATA_END) {
                c->chunk = CH_SIZE;  // the CRLF after a chunk's data
            } else if (c->chunk == CH_TRAILER && len == 0) {
                c->trailing = n > 0;
                body_done(c);
                return false;
            }
        }
        return true;
    case BODY_NONE:
    default:
        c->trailing = true;  // bytes where no body belongs
        return true;
    }
}

// Everything the server sent: status line and headers byte by byte, then body.
static bool feed(struct http_ctx *c, const uint8_t *p, size_t n)
{
    c->got_bytes = true;
    while (n > 0 && c->state != ST_BODY) {
        const char ch = (char)*p++;
        n--;
        if (ch == '\n') {
            c->line[c->line_len] = '\0';
            const size_t len = c->line_len;
            c->line_len = 0;
            if (len == 0 && n > 0) {
                c->trailing = true;  // unless a body takes those bytes (body_bytes decides)
            }
            header_line(c, c->line, len);
            if (c->dead) {
                return false;
            }
        } else if (ch != '\r' && c->line_len < MAX_LINE) {
            c->line[c->line_len++] = ch;
        }
    }
    if (n > 0 && c->state == ST_BODY && !c->dead) {
        return body_bytes(c, p, n);
    }
    return !c->dead;
}

// A kept connection the server had already closed: send the request again
// on a new one.
static bool retry_fresh(struct http_ctx *c)
{
    if (!c->reused || c->got_bytes) {
        return false;
    }
    NSLOG(fetch, INFO, "kept connection to %s was closed, reconnecting", c->host);
    close_conn(c);
    c->reused = false;
    c->req_sent = 0;
    c->line_len = 0;
    c->state = ST_RESOLVE;
    return true;
}

static void closed_by_server(struct http_ctx *c)
{
    if (retry_fresh(c)) {
        return;
    }
    if (c->state == ST_BODY) {
        // End of a read-to-close body; a short Content-Length or chunked body
        // still shows what arrived (curl's fetcher does the same).
        c->conn_close = true;
        body_done(c);
        return;
    }
    if (!c->got_bytes) {
        fail(c, "%s closed the connection without answering", c->host);
    } else {
        fail(c, "%s closed the connection in the middle of the headers", c->host);
    }
}

// The loader could not make the TLS connection: the certificate did not
// verify, or the connection or handshake failed (the device log says which).
// NetSurf shows its certificate error page for this (patches/0008), without
// the button to go on anyway: the loader cannot skip the check.
static void tls_failed(struct http_ctx *c)
{
    NSLOG(fetch, INFO, "TLS connection to %s:%u failed", c->host, (unsigned)c->port);
    finish_with(c, FETCH_CERT_ERR, NULL);
}

// ── One step of a fetch ─────────────────────────────────────────────────────

static void step(struct http_ctx *c, int64_t now)
{
    static uint8_t rbuf[RECV_BUF];
    switch (c->state) {
    case ST_RESOLVE:
        if (c->https && !papp_have_tls()) {
            fail(c, "HTTPS needs a loader with TLS services for apps; this one has none. "
                    "Try the http:// address, or update the loader");
            return;
        }
        if (papp_svc->net_tcp_connect == NULL || papp_svc->net_resolve == NULL) {
            fail(c, "This loader has no network services for apps");
            return;
        }
        // A kept connection to the same server skips the connect (never for
        // a POST: resending one on a stale connection could post twice).
        if (!c->post && !c->proxy) {
            const int h = keep_take(c->tp, c->host, c->port);
            if (h >= 0) {
                c->sock = h;
                c->reused = true;
                c->state = ST_SEND;
                c->deadline = now + IDLE_TIMEOUT_US;
                break;
            }
        }
        if (c->dns < 0) {
            if (parse_ipv4(c->host, &c->ip) || dns_cached(c->host, &c->ip)) {
                c->state = ST_CONNECT;
            } else {
                c->dns = dns_start(c->host);
                if (c->dns >= 0) {
                    progress(c, "Looking up %s", c->host);
                }
                return;
            }
        } else {
            const int r = dns_result(c->dns, &c->ip);
            if (r == 0) {
                return;
            }
            c->dns = -1;
            if (r < 0) {
                fail(c, "Could not find the server %s", c->host);
                return;
            }
            dns_remember(c->host, c->ip);
            c->state = ST_CONNECT;
        }
        c->retry_until = now + CONNECT_RETRY_US;
        c->queued_until = now + WAIT_FOR_SESSION_US;
        break;
    default:
        break;
    }

    if (c->state == ST_CONNECT) {
        const int h = c->tp->open(c->host, c->ip, c->port);
        if (h == -2) {
            // Every TLS session is in use by other fetches: wait for one.
            if (now > c->queued_until) {
                timed_out(c, "Waiting for a secure connection to %s timed out");
            }
            return;
        }
        if (h < 0) {
            if (now < c->retry_until) {
                return;  // the loader has no free socket or session yet
            }
            fail(c, c->https ? "The loader could not open a secure connection to %s "
                               "(no certificate bundle, or no free TLS session)"
                             : "Could not connect to %s", c->host);
            return;
        }
        c->sock = h;
        c->state = ST_CONNECTING;
        c->deadline = now + (c->tp->tls ? TLS_CONNECT_TIMEOUT_US
                                        : (int64_t)nsoption_uint(curl_fetch_timeout) * 1000000);
        progress(c, c->https ? "Securely connecting to %s" : "Connecting to %s", c->host);
        if (c->dead) {
            return;
        }
    }

    if (c->state == ST_CONNECTING) {
        const int r = c->tp->ready(c->sock);
        if (r < 0) {
            if (c->tp->tls) {
                tls_failed(c);
            } else {
                fail(c, "Could not connect to %s:%u", c->host, (unsigned)c->port);
            }
            return;
        }
        if (r == 0) {
            if (now > c->deadline) {
                timed_out(c, "Connecting to %s timed out");
            }
            return;
        }
        c->state = ST_SEND;
    }

    if (c->state == ST_SEND) {
        while (c->req_sent < c->req_len) {
            const int n = c->tp->send(c->sock, c->req + c->req_sent, (int)(c->req_len - c->req_sent));
            if (n < 0) {
                if (!retry_fresh(c)) {
                    fail(c, "Lost the connection to %s", c->host);
                }
                return;
            }
            if (n == 0) {
                break;
            }
            c->req_sent += (size_t)n;
        }
        if (c->req_sent < c->req_len) {
            if (now > c->deadline) {
                timed_out(c, "Sending to %s timed out");
            }
            return;
        }
        c->state = ST_STATUS;
        c->deadline = now + IDLE_TIMEOUT_US;
    }

    // ST_STATUS, ST_HEADERS, ST_BODY
    size_t total = 0;
    while (total < MAX_READ_PER_POLL && !c->dead) {
        const int n = c->tp->recv(c->sock, rbuf, sizeof(rbuf));
        if (n == -2) {
            break;
        }
        if (n < 0) {
            if (!retry_fresh(c)) {
                fail(c, "Lost the connection to %s", c->host);
            }
            return;
        }
        if (n == 0) {
            closed_by_server(c);
            return;
        }
        c->deadline = now + IDLE_TIMEOUT_US;
        total += (size_t)n;
        if (!feed(c, rbuf, (size_t)n)) {
            return;
        }
    }
    if (!c->dead && now > c->deadline) {
        timed_out(c, "%s stopped sending");
    }
}

// ── Fetcher operations ──────────────────────────────────────────────────────

static bool http_initialise(lwc_string *scheme)
{
    (void)scheme;
    return true;
}

static void http_finalise(lwc_string *scheme)
{
    (void)scheme;
}

static bool http_acceptable(const nsurl *url)
{
    (void)url;
    return true;
}

static void *http_setup(struct fetch *parent, nsurl *url, bool only_2xx, bool downgrade_tls, const char *post_urlenc,
                        const struct fetch_multipart_data *post_multipart, const char **headers)
{
    (void)downgrade_tls;  // the loader's TLS has no weaker mode to fall back to
    struct http_ctx *c = calloc(1, sizeof(*c));
    if (c == NULL) {
        return NULL;
    }
    c->fetch = parent;
    c->url = nsurl_ref(url);
    c->only_2xx = only_2xx;
    c->https = nsurl_get_scheme_type(url) == NSURL_SCHEME_HTTPS;
    c->tp = c->https ? &tls_transport : &tcp_transport;
    c->dns = -1;
    c->sock = -1;
    c->content_length = -1;
    c->state = ST_RESOLVE;
    if (!build_request(c, post_urlenc, post_multipart, headers)) {
        nsurl_unref(c->url);
        free(c);
        return NULL;
    }
    // Keep fetches in the order NetSurf started them.
    struct http_ctx **tail = &s_list;
    while (*tail != NULL) {
        tail = &(*tail)->next;
    }
    *tail = c;
    return c;
}

static bool http_start(void *vctx)
{
    struct http_ctx *c = vctx;
    c->started = true;
    return true;
}

static void release(struct http_ctx *c)
{
    close_conn(c);
    dns_abandon(c->dns);
    c->dns = -1;
}

static void http_abort(void *vctx)
{
    struct http_ctx *c = vctx;
    if (s_polling || c->locked) {
        c->aborted = true;  // the poll loop cleans up after itself
        return;
    }
    release(c);
    fetch_remove_from_queues(c->fetch);
    fetch_free(c->fetch);
}

static void http_free(void *vctx)
{
    struct http_ctx *c = vctx;
    release(c);
    for (struct http_ctx **p = &s_list; *p != NULL; p = &(*p)->next) {
        if (*p == c) {
            *p = c->next;
            break;
        }
    }
    if (c->zinit) {
        inflateEnd(&c->z);
    }
    nsurl_unref(c->url);
    free(c->req);
    free(c->location);
    free(c->realm);
    free(c);
}

static void http_poll(lwc_string *scheme)
{
    bool match = false;
    // Registered for http and https: do the work once per scheduler tick.
    if (lwc_string_isequal(scheme, corestring_lwc_http, &match) != lwc_error_ok || !match) {
        return;
    }
    s_polling = true;
    const int64_t now = papp_time_us();
    keep_check(now);
    for (struct http_ctx *c = s_list; c != NULL; c = c->next) {
        if (c->dead) {
            continue;
        }
        if (c->aborted) {
            c->dead = true;
            continue;
        }
        if (c->started) {
            step(c, now);
        }
    }
    s_polling = false;
    // Finished, failed and aborted fetches go back to NetSurf.
    for (;;) {
        struct http_ctx *c = s_list;
        while (c != NULL && !c->dead && !c->aborted) {
            c = c->next;
        }
        if (c == NULL) {
            break;
        }
        release(c);
        fetch_remove_from_queues(c->fetch);
        fetch_free(c->fetch);  // calls http_free
    }
    dns_reap();
}

nserror papp_fetch_http_register(void)
{
    const struct fetcher_operation_table ops = {
        .initialise = http_initialise,
        .acceptable = http_acceptable,
        .setup = http_setup,
        .start = http_start,
        .abort = http_abort,
        .free = http_free,
        .poll = http_poll,
        .fdset = NULL,
        .finalise = http_finalise,
    };
    nserror err = fetcher_add(lwc_string_ref(corestring_lwc_http), &ops);
    if (err != NSERROR_OK) {
        return err;
    }
    return fetcher_add(lwc_string_ref(corestring_lwc_https), &ops);
}
