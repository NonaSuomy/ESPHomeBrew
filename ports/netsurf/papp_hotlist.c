// Bookmarks and history for the NetSurf PAPP.
//
// Bookmarks are NetSurf's hotlist (desktop/hotlist.c), read from and saved
// to PAPP_NS_DIR/Hotlist in NetSurf's own format (an HTML list). The
// framebuffer frontend has no hotlist window, so patches/0013 adds:
//   - the toolbar star, or Ctrl-D: bookmark the page, or remove its bookmark
//     (the star is filled while the page is bookmarked),
//   - the toolbar list button, or Ctrl-B: about:hotlist, the bookmarks as
//     links, each with a link that removes it (about:hotlist?remove=URL),
//   - Ctrl-H: about:history, the pages visited, newest first, from
//     NetSurf's URL database. That is kept in PAPP_NS_DIR/URLs between runs.
// NetSurf's own local history (the toolbar's history button: this window's
// back and forward pages) is unchanged.
//
// The loader cannot create folders, so without PAPP_NS_DIR on the card both
// last until NetSurf quits: the saves fail quietly (patches/0013 makes the
// hotlist save write the file in place; the loader cannot rename files).
#include "papp_port.h"

#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "utils/errors.h"
#include "utils/log.h"
#include "utils/nsurl.h"
#include "utils/url.h"
#include "netsurf/url_db.h"
#include "desktop/hotlist.h"
#include "content/fetchers/about/private.h"

#define HOTLIST_FILE PAPP_NS_DIR "/Hotlist"
#define URLS_FILE PAPP_NS_DIR "/URLs"
#define HISTORY_MAX 300  // pages on about:history

void papp_netsurf_started(void);
void papp_netsurf_stopping(void);
bool papp_hotlist_has(struct nsurl *url);
bool papp_hotlist_toggle(struct nsurl *url);
bool fetch_about_papp_hotlist_handler(struct fetch_about_context *ctx);
bool fetch_about_papp_history_handler(struct fetch_about_context *ctx);

static bool s_hotlist = false;  // hotlist_init worked

// frontends/framebuffer/gui.c main() (patches/0013), once NetSurf is up.
void papp_netsurf_started(void)
{
    if (papp_file_exists(URLS_FILE)) {
        urldb_load(URLS_FILE);
    }
    const bool saved = papp_file_exists(HOTLIST_FILE);
    const nserror err = hotlist_init(HOTLIST_FILE, HOTLIST_FILE);
    s_hotlist = err == NSERROR_OK;
    papp_svc->log_printf("NETSURF: bookmarks %s\n",
                         !s_hotlist ? "failed to start" : saved ? "loaded from " HOTLIST_FILE : "not saved yet");
}

// Before netsurf_exit(): save what can be saved.
void papp_netsurf_stopping(void)
{
    if (urldb_save(URLS_FILE) != NSERROR_OK) {
        NSLOG(netsurf, INFO, "history not saved (no %s?)", PAPP_NS_DIR);
    }
    if (s_hotlist) {
        hotlist_fini();
        s_hotlist = false;
    }
}

bool papp_hotlist_has(struct nsurl *url)
{
    return s_hotlist && hotlist_has_url(url);
}

// Bookmark the page or remove its bookmark; true when it is bookmarked now.
bool papp_hotlist_toggle(struct nsurl *url)
{
    if (!s_hotlist) {
        return false;
    }
    if (hotlist_has_url(url)) {
        hotlist_remove_url(url);
        papp_svc->log_printf("NETSURF: bookmark removed\n");
        return false;
    }
    const bool added = hotlist_add_url(url) == NSERROR_OK;
    papp_svc->log_printf("NETSURF: bookmark %s\n", added ? "added" : "not added");
    return added;
}

// ── Page output ─────────────────────────────────────────────────────────────

// Text with &, <, > and " escaped.
static nserror send_text(struct fetch_about_context *ctx, const char *s)
{
    char buf[256];
    size_t n = 0;
    nserror res = NSERROR_OK;
    for (; s != NULL && *s != '\0' && res == NSERROR_OK; s++) {
        const char *rep = NULL;
        switch (*s) {
        case '&':
            rep = "&amp;";
            break;
        case '<':
            rep = "&lt;";
            break;
        case '>':
            rep = "&gt;";
            break;
        case '"':
            rep = "&quot;";
            break;
        default:
            break;
        }
        const size_t len = rep != NULL ? strlen(rep) : 1;
        if (n + len > sizeof(buf)) {
            res = fetch_about_senddata(ctx, (const uint8_t *)buf, n);
            n = 0;
        }
        if (rep != NULL) {
            memcpy(buf + n, rep, len);
        } else {
            buf[n] = *s;
        }
        n += len;
    }
    if (res == NSERROR_OK && n > 0) {
        res = fetch_about_senddata(ctx, (const uint8_t *)buf, n);
    }
    return res;
}

static bool page_start(struct fetch_about_context *ctx, const char *title)
{
    fetch_about_set_http_code(ctx, 200);
    if (fetch_about_send_header(ctx, "Content-Type: text/html; charset=utf-8")) {
        return false;
    }
    return fetch_about_ssenddataf(ctx,
                                  "<!DOCTYPE html>\n<html>\n<head>\n<meta charset=\"utf-8\">\n"
                                  "<title>%s</title>\n"
                                  "<link rel=\"stylesheet\" type=\"text/css\" href=\"resource:internal.css\">\n"
                                  "<style>li { margin: 0.6em 0; } .url { color: #666; font-size: 85%%; }"
                                  " .remove { font-size: 85%%; margin-left: 1em; }</style>\n"
                                  "</head>\n<body class=\"ns-even-bg ns-even-fg ns-border\">\n"
                                  "<h1 class=\"ns-border\">%s</h1>\n",
                                  title, title) == NSERROR_OK;
}

static bool page_end(struct fetch_about_context *ctx)
{
    if (fetch_about_ssenddataf(ctx, "</body>\n</html>\n") != NSERROR_OK) {
        return false;
    }
    fetch_about_send_finished(ctx);
    return true;
}

// The value of `key` in the page's query (unescaped, caller frees), or NULL.
static char *query_value(struct fetch_about_context *ctx, const char *key)
{
    char *query = NULL;
    size_t len = 0;
    char *value = NULL;
    if (nsurl_get(fetch_about_get_url(ctx), NSURL_QUERY, &query, &len) != NSERROR_OK) {
        return NULL;
    }
    const size_t klen = strlen(key);
    const char *p = query;
    const char *end = query + len;
    if (p < end && *p == '?') {
        p++;
    }
    while (p < end && value == NULL) {
        const char *amp = memchr(p, '&', (size_t)(end - p));
        const char *stop = amp != NULL ? amp : end;
        if ((size_t)(stop - p) > klen && strncmp(p, key, klen) == 0 && p[klen] == '=') {
            if (url_unescape(p + klen + 1, (size_t)(stop - p - klen - 1), NULL, &value) != NSERROR_OK) {
                value = NULL;
            }
        }
        p = stop + 1;
    }
    free(query);
    return value;
}

// ── about:hotlist ───────────────────────────────────────────────────────────

struct hotlist_page {
    struct fetch_about_context *ctx;
    int entries;
    nserror res;
};

static nserror hotlist_folder_enter(void *vctx, const char *title)
{
    struct hotlist_page *hp = vctx;
    if (hp->res == NSERROR_OK) {
        hp->res = fetch_about_ssenddataf(hp->ctx, "<li><b>");
    }
    if (hp->res == NSERROR_OK) {
        hp->res = send_text(hp->ctx, title);
    }
    if (hp->res == NSERROR_OK) {
        hp->res = fetch_about_ssenddataf(hp->ctx, "</b>\n<ul>\n");
    }
    return NSERROR_OK;
}

static nserror hotlist_address(void *vctx, struct nsurl *url, const char *title)
{
    struct hotlist_page *hp = vctx;
    const char *address = nsurl_access(url);
    char *escaped = NULL;
    hp->entries++;
    if (hp->res == NSERROR_OK) {
        hp->res = fetch_about_ssenddataf(hp->ctx, "<li><a href=\"");
    }
    if (hp->res == NSERROR_OK) {
        hp->res = send_text(hp->ctx, address);
    }
    if (hp->res == NSERROR_OK) {
        hp->res = fetch_about_ssenddataf(hp->ctx, "\">");
    }
    if (hp->res == NSERROR_OK) {
        hp->res = send_text(hp->ctx, title != NULL && title[0] != '\0' ? title : address);
    }
    if (hp->res == NSERROR_OK && url_escape(address, false, NULL, &escaped) == NSERROR_OK) {
        hp->res = fetch_about_ssenddataf(hp->ctx, "</a><a class=\"remove\" href=\"about:hotlist?remove=");
        if (hp->res == NSERROR_OK) {
            hp->res = send_text(hp->ctx, escaped);
        }
        if (hp->res == NSERROR_OK) {
            hp->res = fetch_about_ssenddataf(hp->ctx, "\">remove</a>");
        }
        free(escaped);
    }
    if (hp->res == NSERROR_OK) {
        hp->res = fetch_about_ssenddataf(hp->ctx, "<br><span class=\"url\">");
    }
    if (hp->res == NSERROR_OK) {
        hp->res = send_text(hp->ctx, address);
    }
    if (hp->res == NSERROR_OK) {
        hp->res = fetch_about_ssenddataf(hp->ctx, "</span></li>\n");
    }
    return NSERROR_OK;
}

static nserror hotlist_folder_leave(void *vctx)
{
    struct hotlist_page *hp = vctx;
    if (hp->res == NSERROR_OK) {
        hp->res = fetch_about_ssenddataf(hp->ctx, "</ul>\n</li>\n");
    }
    return NSERROR_OK;
}

bool fetch_about_papp_hotlist_handler(struct fetch_about_context *ctx)
{
    char *remove = query_value(ctx, "remove");
    if (remove != NULL) {
        nsurl *url;
        if (s_hotlist && nsurl_create(remove, &url) == NSERROR_OK) {
            hotlist_remove_url(url);
            nsurl_unref(url);
        }
        free(remove);
        return fetch_about_redirect(ctx, "about:hotlist");
    }

    if (!page_start(ctx, "Bookmarks")) {
        return false;
    }
    struct hotlist_page hp = {ctx, 0, NSERROR_OK};
    if (!s_hotlist) {
        hp.res = fetch_about_ssenddataf(ctx, "<p>The bookmarks could not be loaded (see the device log).</p>\n");
    } else {
        hp.res = fetch_about_ssenddataf(ctx, "<ul>\n");
        hotlist_iterate(&hp, hotlist_folder_enter, hotlist_address, hotlist_folder_leave);
        if (hp.res == NSERROR_OK) {
            hp.res = fetch_about_ssenddataf(ctx, "</ul>\n");
        }
        if (hp.res == NSERROR_OK && hp.entries == 0) {
            hp.res = fetch_about_ssenddataf(ctx, "<p>No bookmarks yet.</p>\n");
        }
    }
    if (hp.res == NSERROR_OK) {
        hp.res = fetch_about_ssenddataf(ctx,
                                        "<p>The star in the toolbar (or Ctrl-D) bookmarks the page you are on, "
                                        "or removes its bookmark. Bookmarks are saved in <code>%s</code> "
                                        "when the folder <code>%s</code> is on the card; without it they last "
                                        "until NetSurf quits.</p>\n"
                                        "<p><a href=\"about:history\">Pages visited</a></p>\n",
                                        HOTLIST_FILE, PAPP_NS_DIR);
    }
    return hp.res == NSERROR_OK && page_end(ctx);
}

// ── about:history ───────────────────────────────────────────────────────────

struct visit {
    nsurl *url;
    char *title;
    time_t when;
};

static struct visit *s_visits;
static size_t s_count, s_cap;

static bool collect_visit(struct nsurl *url, const struct url_data *data)
{
    if (data == NULL || data->visits == 0) {
        return true;
    }
    if (s_count == s_cap) {
        const size_t cap = s_cap ? s_cap * 2 : 128;
        struct visit *bigger = realloc(s_visits, cap * sizeof(*bigger));
        if (bigger == NULL) {
            return false;
        }
        s_visits = bigger;
        s_cap = cap;
    }
    s_visits[s_count].url = nsurl_ref(url);
    s_visits[s_count].title = data->title != NULL ? strdup(data->title) : NULL;
    s_visits[s_count].when = data->last_visit;
    s_count++;
    return true;
}

static int newest_first(const void *a, const void *b)
{
    const struct visit *va = a, *vb = b;
    if (va->when != vb->when) {
        return va->when < vb->when ? 1 : -1;
    }
    return strcmp(nsurl_access(va->url), nsurl_access(vb->url));
}

bool fetch_about_papp_history_handler(struct fetch_about_context *ctx)
{
    s_count = 0;
    urldb_iterate_entries(collect_visit);
    qsort(s_visits, s_count, sizeof(*s_visits), newest_first);

    nserror res = page_start(ctx, "Pages visited") ? NSERROR_OK : NSERROR_INVALID;
    char day[16] = "";
    bool open = false;
    for (size_t i = 0; i < s_count && i < HISTORY_MAX && res == NSERROR_OK; i++) {
        const struct visit *v = &s_visits[i];
        char this_day[16] = "";
        char clock[8] = "";
        struct tm tm;
        // The clock is only right once a web server has sent the date.
        if (v->when > (time_t)978307200 && gmtime_r(&v->when, &tm) != NULL) {
            strftime(this_day, sizeof(this_day), "%Y-%m-%d", &tm);
            strftime(clock, sizeof(clock), "%H:%M", &tm);
        } else {
            strcpy(this_day, "time unknown");
        }
        if (!open || strcmp(day, this_day) != 0) {
            res = fetch_about_ssenddataf(ctx, "%s<h2>%s</h2>\n<ul>\n", open ? "</ul>\n" : "", this_day);
            snprintf(day, sizeof(day), "%s", this_day);
            open = true;
        }
        const char *address = nsurl_access(v->url);
        if (res == NSERROR_OK) {
            res = fetch_about_ssenddataf(ctx, "<li>%s%s<a href=\"", clock, clock[0] ? " " : "");
        }
        if (res == NSERROR_OK) {
            res = send_text(ctx, address);
        }
        if (res == NSERROR_OK) {
            res = fetch_about_ssenddataf(ctx, "\">");
        }
        if (res == NSERROR_OK) {
            res = send_text(ctx, v->title != NULL && v->title[0] != '\0' ? v->title : address);
        }
        if (res == NSERROR_OK) {
            res = fetch_about_ssenddataf(ctx, "</a><br><span class=\"url\">");
        }
        if (res == NSERROR_OK) {
            res = send_text(ctx, address);
        }
        if (res == NSERROR_OK) {
            res = fetch_about_ssenddataf(ctx, "</span></li>\n");
        }
    }
    if (res == NSERROR_OK && open) {
        res = fetch_about_ssenddataf(ctx, "</ul>\n");
    }
    if (res == NSERROR_OK) {
        res = fetch_about_ssenddataf(ctx,
                                     "%s<p>The pages NetSurf has visited, newest first (at most %d; times in "
                                     "UTC). They are kept in <code>%s</code> when the folder <code>%s</code> "
                                     "is on the card.</p>\n<p><a href=\"about:hotlist\">Bookmarks</a></p>\n",
                                     s_count == 0 ? "<p>No pages visited yet.</p>\n" : "", HISTORY_MAX, URLS_FILE,
                                     PAPP_NS_DIR);
    }
    for (size_t i = 0; i < s_count; i++) {
        nsurl_unref(s_visits[i].url);
        free(s_visits[i].title);
    }
    s_count = 0;
    return res == NSERROR_OK && page_end(ctx);
}
