#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "https.h"
#include "fsu8.h"

/* one attempt found its reused connection already closed by the server
 * (keep-alive timeout) before any byte of the answer: worth a new one */
#define STALE 1

struct https_client {
    tls_conn *tls;
    tls_info info;
    int net_err;
    volatile int canceled;
    /* the open connection */
    int open, is_tls, port, reusable;
    char host[256];
    /* bytes received and not yet used */
    unsigned char rbuf[8192];
    size_t rpos, rlen;
};

/* ---- small text helpers ---- */

static int lower(int ch) { return ch >= 'A' && ch <= 'Z' ? ch + 32 : ch; }

static int same_ci(const char *a, const char *b)
{
    for (; *a && *b; a++, b++) if (lower((unsigned char)*a) != lower((unsigned char)*b)) return 0;
    return *a == *b;
}

static int prefix_ci(const char *s, const char *p)
{
    for (; *p; s++, p++) if (lower((unsigned char)*s) != lower((unsigned char)*p)) return 0;
    return 1;
}

static int ends_ci(const char *s, const char *suffix)
{
    size_t ls = strlen(s), lx = strlen(suffix);
    return ls >= lx && same_ci(s + ls - lx, suffix);
}

void https_pct(strbuf *out, const char *s, const char *keep)
{
    static const char hx[] = "0123456789ABCDEF";
    for (; *s; s++) {
        unsigned char ch = (unsigned char)*s;
        char one[4];
        if ((ch >= 'A' && ch <= 'Z') || (ch >= 'a' && ch <= 'z') || (ch >= '0' && ch <= '9') ||
            strchr("-._~", ch) || (keep && strchr(keep, ch))) {
            one[0] = (char)ch; one[1] = 0;
        } else {
            one[0] = '%'; one[1] = hx[ch >> 4]; one[2] = hx[ch & 15]; one[3] = 0;
        }
        sb_puts(out, one);
    }
}

void https_form(strbuf *out, const char *name, const char *value)
{
    if (out->len) sb_puts(out, "&");
    https_pct(out, name, NULL);
    sb_puts(out, "=");
    https_pct(out, value, NULL);
}

/* ---- URLs and what is allowed ---- */

int https_test_mode(void)
{
#ifdef RA_TEST_BUILD
    return 1;
#else
    const char *e = getenv("RETROARCHIVER_IA_TEST");
    return e && !strcmp(e, "1");
#endif
}

/* no spaces or control characters: they would end up in the request line */
static int clean_url(const char *s)
{
    for (; *s; s++) if ((unsigned char)*s <= 0x20 || *s == 0x7f) return 0;
    return 1;
}

int https_parse_url(const char *url, https_url *u)
{
    const char *p, *end; size_t hl;
    memset(u, 0, sizeof(*u));
    if (!url || !clean_url(url)) return 0;
    if (prefix_ci(url, "https://")) { u->tls = 1; u->port = 443; p = url + 8; }
    else if (prefix_ci(url, "http://")) { u->tls = 0; u->port = 80; p = url + 7; }
    else return 0;
    end = p + strcspn(p, ":/?#");
    hl = (size_t)(end - p);
    if (hl == 0 || hl >= sizeof(u->host) || memchr(p, '@', hl)) return 0;
    memcpy(u->host, p, hl);
    p = end;
    if (*p == ':') {
        char *e; long port = strtol(p + 1, &e, 10);
        if (e == p + 1 || port < 1 || port > 65535) return 0;
        u->port = (int)port;
        p = e;
    }
    if (*p == '#') p += strlen(p);
    if (*p && *p != '/' && *p != '?') return 0;
    end = p + strcspn(p, "#");
    if ((size_t)(end - p) + 2 > sizeof(u->path)) return 0;
    if (*p != '/') strcpy(u->path, "/");
    strncat(u->path, p, (size_t)(end - p));
    return 1;
}

static int is_loopback(const char *host)
{
    return !strcmp(host, "127.0.0.1") || same_ci(host, "localhost");
}

int https_url_allowed(const https_url *u)
{
    if (u->tls) return 1;
    return https_test_mode() && !strcmp(u->host, "127.0.0.1");
}

int https_redirect_allowed(const https_url *from, const https_url *to)
{
    if (to->tls && (same_ci(to->host, "archive.org") || ends_ci(to->host, ".archive.org")))
        return 1;
    /* the fake server can only redirect to itself */
    return https_test_mode() && is_loopback(to->host) && to->tls == from->tls &&
           to->port == from->port && same_ci(to->host, from->host);
}

/* ---- the connection ---- */

https_client *https_new(const tls_config *cfg)
{
    https_client *c = (https_client *)calloc(1, sizeof(*c));
    if (!c) return NULL;
    c->tls = tls_new(cfg);
    if (!c->tls) { free(c); return NULL; }
    return c;
}

void https_disconnect(https_client *c)
{
    tls_close(c->tls);
    c->open = 0;
    c->rpos = c->rlen = 0;
}

void https_free(https_client *c)
{
    if (!c) return;
    https_disconnect(c);
    tls_free(c->tls);
    free(c);
}

void https_cancel(https_client *c)
{
    c->canceled = 1;
    net_cancel(tls_net(c->tls));
}

void https_uncancel(https_client *c)
{
    c->canceled = 0;
    net_uncancel(tls_net(c->tls));
}

const tls_info *https_tls_info(const https_client *c) { return &c->info; }
int https_net_error(const https_client *c) { return c->net_err; }

static int net_fail(https_client *c, int r)
{
    c->net_err = r;
    c->reusable = 0;
    return r == NET_ERR_CANCELED || c->canceled ? HTTPS_ERR_CANCELED : HTTPS_ERR_NET;
}

static int connect_to(https_client *c, const https_url *u)
{
    int r;
    https_disconnect(c);
    memset(&c->info, 0, sizeof(c->info));
    if (u->tls) {
        r = tls_open(c->tls, u->host, u->port, &c->info);
        if (r == TLS_ERR_CANCELED) return HTTPS_ERR_CANCELED;
        if (r == TLS_ERR_NET) return net_fail(c, c->info.net_err);
        if (r != TLS_OK) return HTTPS_ERR_TLS;
    } else {
        r = net_connect(tls_net(c->tls), u->host, u->port);
        if (r != NET_OK) return net_fail(c, r);
    }
    c->open = 1; c->is_tls = u->tls; c->port = u->port; c->reusable = 1;
    str_copy(c->host, sizeof(c->host), u->host);
    return HTTPS_OK;
}

static int can_reuse(const https_client *c, const https_url *u)
{
    return c->open && c->reusable && c->is_tls == u->tls && c->port == u->port &&
           same_ci(c->host, u->host) && net_connected(tls_net(c->tls));
}

static int c_write(https_client *c, const void *buf, size_t n)
{
    return c->is_tls ? tls_write(c->tls, buf, n) : net_send(tls_net(c->tls), buf, n);
}

static int c_flush(https_client *c)
{
    return c->is_tls ? tls_flush(c->tls) : NET_OK;
}

/* new bytes into rbuf: > 0, 0 if none within wait_ms (>= 0), or NET_ERR_* */
static int c_fill(https_client *c, int wait_ms)
{
    int r;
    if (c->is_tls) r = wait_ms >= 0 ? tls_read_wait(c->tls, c->rbuf, sizeof(c->rbuf), wait_ms)
                                    : tls_read(c->tls, c->rbuf, sizeof(c->rbuf));
    else r = wait_ms >= 0 ? net_recv_wait(tls_net(c->tls), c->rbuf, sizeof(c->rbuf), wait_ms)
                          : net_recv(tls_net(c->tls), c->rbuf, sizeof(c->rbuf));
    if (r > 0) { c->rpos = 0; c->rlen = (size_t)r; }
    return r;
}

/* a line without its CRLF; NET_OK or NET_ERR_* */
static int read_line(https_client *c, strbuf *line)
{
    sb_clear(line);
    for (;;) {
        char *s, *nl; size_t avail;
        if (c->rpos == c->rlen) { int r = c_fill(c, -1); if (r < 0) return r; }
        s = (char *)c->rbuf + c->rpos;
        avail = c->rlen - c->rpos;
        nl = (char *)memchr(s, '\n', avail);
        if (nl) avail = (size_t)(nl - s) + 1;
        sb_add(line, s, avail);
        c->rpos += avail;
        if (line->len > 16384) return NET_ERR_IO;     /* not an HTTP header */
        if (nl) break;
    }
    while (line->len && (line->s[line->len - 1] == '\n' || line->s[line->len - 1] == '\r'))
        line->s[--line->len] = 0;
    return NET_OK;
}

/* n more bytes of the body; out of the limit: HTTPS_ERR_TOOBIG */
static int read_n(https_client *c, long long n, strbuf *out)
{
    if (out->len + n > (unsigned long long)HTTPS_MAX_BODY) return HTTPS_ERR_TOOBIG;
    while (n > 0) {
        size_t take;
        if (c->rpos == c->rlen) { int r = c_fill(c, -1); if (r < 0) return net_fail(c, r); }
        take = c->rlen - c->rpos;
        if ((long long)take > n) take = (size_t)n;
        sb_add(out, c->rbuf + c->rpos, take);
        c->rpos += take; n -= (long long)take;
    }
    return HTTPS_OK;
}

/* ---- the request ---- */

static long long body_length(const https_request *rq)
{
    long long size;
    if (!rq->body_file) return rq->body ? (long long)rq->body_len : 0;
    size = u8_filesize(rq->body_file);
    if (size < 0 || rq->file_offset < 0 || rq->file_offset > size) return -1;
    if (rq->file_len <= 0) return size - rq->file_offset;
    return rq->file_offset + rq->file_len > size ? -1 : rq->file_len;
}

static int has_body(const https_request *rq)
{
    return rq->body || rq->body_file || !strcmp(rq->method, "PUT") || !strcmp(rq->method, "POST");
}

static void build_head(strbuf *h, const https_request *rq, const https_url *u, long long blen)
{
    sb_printf(h, "%s %s HTTP/1.1\r\nHost: %s", rq->method, u->path, u->host);
    if (u->port != (u->tls ? 443 : 80)) sb_printf(h, ":%d", u->port);
    sb_puts(h, "\r\n");
    if (has_body(rq)) sb_printf(h, "Content-Length: %lld\r\n", blen);
    if (rq->expect_continue && blen > 0) sb_puts(h, "Expect: 100-continue\r\n");
    if (rq->headers) sb_puts(h, rq->headers);
    sb_puts(h, "\r\n");
}

/* 64-bit position without _fseeki64, which XP's msvcrt.dll does not export */
static int seek_to(FILE *f, long long off)
{
#ifdef _WIN32
    fpos_t pos = (fpos_t)off;
    return fsetpos(f, &pos) == 0;
#else
    return fseeko(f, (off_t)off, SEEK_SET) == 0;
#endif
}

static int send_piece(https_client *c, const https_request *rq, long long off, const void *p, size_t n)
{
    int r;
    if (c->canceled) return HTTPS_ERR_CANCELED;
    if (rq->on_body && rq->on_body(rq->ctx, off, p, n)) { c->reusable = 0; return HTTPS_ERR_CANCELED; }
    r = c_write(c, p, n);
    return r == NET_OK ? HTTPS_OK : net_fail(c, r);
}

static int send_file(https_client *c, const https_request *rq, long long blen)
{
    FILE *f = u8_fopen(rq->body_file, "rb");
    unsigned char *buf = (unsigned char *)malloc(HTTPS_PIECE);
    long long off = 0; int r = HTTPS_OK;
    if (!f || !buf || !seek_to(f, rq->file_offset)) r = HTTPS_ERR_FILE;
    while (r == HTTPS_OK && off < blen) {
        size_t want = blen - off > HTTPS_PIECE ? HTTPS_PIECE : (size_t)(blen - off);
        if (fread(buf, 1, want, f) != want) { r = HTTPS_ERR_FILE; break; }
        r = send_piece(c, rq, off, buf, want);
        off += (long long)want;
    }
    if (f) fclose(f);
    free(buf);
    /* the server waits for bytes that will not come: the connection is lost */
    if (r != HTTPS_OK) c->reusable = 0;
    return r;
}

static int send_body(https_client *c, const https_request *rq, long long blen)
{
    int r = HTTPS_OK;
    if (rq->body_file) r = send_file(c, rq, blen);
    else {
        long long off = 0;
        while (r == HTTPS_OK && off < blen) {
            size_t n = blen - off > HTTPS_PIECE ? HTTPS_PIECE : (size_t)(blen - off);
            r = send_piece(c, rq, off, (const char *)rq->body + off, n);
            off += (long long)n;
        }
    }
    if (r == HTTPS_OK && (r = c_flush(c)) != NET_OK) r = net_fail(c, r);
    return r;
}

/* the status line and headers of one response, into rs->head */
static int read_head(https_client *c, https_response *rs)
{
    strbuf line; int r, minor = 0, bad = 0;
    sb_init(&line);
    sb_clear(&rs->head);
    r = read_line(c, &line);
    if (r == NET_OK && (sscanf(line.s, "HTTP/1.%d %d", &minor, &rs->status) != 2 ||
                        rs->status < 100 || rs->status > 999)) bad = 1;
    if (r == NET_OK && minor == 0) c->reusable = 0;   /* HTTP/1.0 closes */
    while (r == NET_OK && !bad) {
        sb_puts(&rs->head, line.s);
        sb_puts(&rs->head, "\r\n");
        if (!line.len) break;
        r = read_line(c, &line);
        if (rs->head.len > 65536) bad = 1;
    }
    sb_free(&line);
    if (bad) { c->reusable = 0; return HTTPS_ERR_PROTO; }
    return r == NET_OK ? HTTPS_OK : net_fail(c, r);
}

static int read_chunked(https_client *c, https_response *rs)
{
    strbuf line; int r = HTTPS_OK;
    sb_init(&line);
    for (;;) {
        char *e; long long size;
        int nr = read_line(c, &line);
        if (nr != NET_OK) { r = net_fail(c, nr); break; }
        size = strtoll(line.s, &e, 16);
        if (e == line.s || size < 0) { r = HTTPS_ERR_PROTO; break; }
        if (size == 0) {
            /* trailers up to the empty line */
            do nr = read_line(c, &line); while (nr == NET_OK && line.len);
            if (nr != NET_OK) r = net_fail(c, nr);
            break;
        }
        if ((r = read_n(c, size, &rs->body)) != HTTPS_OK) break;
        if ((nr = read_line(c, &line)) != NET_OK) { r = net_fail(c, nr); break; }
    }
    sb_free(&line);
    if (r != HTTPS_OK) c->reusable = 0;
    return r;
}

/* without length: the body ends when the server closes */
static int read_to_close(https_client *c, https_response *rs)
{
    c->reusable = 0;
    for (;;) {
        long long n = (long long)(c->rlen - c->rpos);
        int r;
        if (n > 0 && (r = read_n(c, n, &rs->body)) != HTTPS_OK) return r;
        r = c_fill(c, -1);
        if (r == NET_ERR_CLOSED) return HTTPS_OK;
        if (r < 0) return net_fail(c, r);
    }
}

static int read_body(https_client *c, const https_request *rq, https_response *rs)
{
    char v[64];
    if (https_header(rs, "Connection", v, sizeof(v)) && same_ci(v, "close")) c->reusable = 0;
    if (!strcmp(rq->method, "HEAD") || rs->status < 200 || rs->status == 204 || rs->status == 304)
        return HTTPS_OK;
    if (https_header(rs, "Transfer-Encoding", v, sizeof(v)) && ends_ci(v, "chunked"))
        return read_chunked(c, rs);
    if (https_header(rs, "Content-Length", v, sizeof(v))) {
        char *e; long long n = strtoll(v, &e, 10);
        if (e == v || n < 0) { c->reusable = 0; return HTTPS_ERR_PROTO; }
        return read_n(c, n, &rs->body);
    }
    return read_to_close(c, rs);
}

/* Expect: 100-continue. 1: send the body; 0: a final answer came first
 * (in rs->head), and the body must not go */
static int await_continue(https_client *c, https_response *rs, int *send)
{
    int r = c->rpos < c->rlen ? 1 : c_fill(c, HTTPS_CONTINUE_MS);
    *send = 1;
    if (r == 0) return HTTPS_OK;                 /* silent server: send anyway */
    if (r < 0) return net_fail(c, r);
    if ((r = read_head(c, rs)) != HTTPS_OK) return r;
    if (rs->status != 100) { *send = 0; c->reusable = 0; }
    return HTTPS_OK;
}

/* one request on one connection, without redirects */
static int attempt(https_client *c, const https_request *rq, const https_url *u,
                   long long blen, https_response *rs)
{
    strbuf head; int r, send = 1, got = 0;
    rs->reused = can_reuse(c, u);
    if (!rs->reused && (r = connect_to(c, u)) != HTTPS_OK) return r;
    c->rpos = c->rlen = 0;
    sb_init(&head);
    build_head(&head, rq, u, blen);
    r = c_write(c, head.s, head.len);
    sb_free(&head);
    if (r == NET_OK && rq->expect_continue && blen > 0) {
        r = c_flush(c);
        if (r == NET_OK) {
            r = await_continue(c, rs, &send);
            got = 1;
            if (r != HTTPS_OK) return r;
        }
    }
    if (r != NET_OK) return rs->reused && r != NET_ERR_CANCELED ? STALE : net_fail(c, r);
    if (send && (r = send_body(c, rq, blen)) != HTTPS_OK) return r;
    if (send) {
        do {
            if (!got && c->rpos == c->rlen) {
                int f = c_fill(c, -1);
                if (f < 0) return rs->reused && f != NET_ERR_CANCELED && f != NET_ERR_TIMEOUT ? STALE : net_fail(c, f);
            }
            got = 1;
            if ((r = read_head(c, rs)) != HTTPS_OK) return r;
        } while (rs->status >= 100 && rs->status < 200);
    }
    sb_clear(&rs->body);
    return read_body(c, rq, rs);
}

/* a Location to follow, absolute or as a path on the same server */
static int next_url(const https_url *u, const char *loc, char *out, size_t n)
{
    if (loc[0] == '/' && loc[1] != '/')
        snprintf(out, n, "%s://%s:%d%s", u->tls ? "https" : "http", u->host, u->port, loc);
    else str_copy(out, n, loc);
    return (int)strlen(out) < (int)n - 1;
}

int https_do(https_client *c, const https_request *rq, https_response *rs)
{
    char url[2400], loc[2048];
    long long blen = body_length(rq);
    int hops;
    sb_clear(&rs->head); sb_clear(&rs->body);
    rs->status = 0; rs->redirects = 0; rs->reused = 0;
    c->net_err = 0;
    if (blen < 0) return HTTPS_ERR_FILE;
    str_copy(url, sizeof(url), rq->url);
    for (hops = 0; ; hops++) {
        https_url u, to; int r;
        if (c->canceled) return HTTPS_ERR_CANCELED;
        if (!https_parse_url(url, &u) || !https_url_allowed(&u)) return HTTPS_ERR_URL;
        r = attempt(c, rq, &u, blen, rs);
        /* a new connection cannot be stale: the second attempt says how it went */
        if (r == STALE) { https_disconnect(c); r = attempt(c, rq, &u, blen, rs); }
        /* after a failure the connection is in an unknown state */
        if (r != HTTPS_OK || !c->reusable) https_disconnect(c);
        if (r != HTTPS_OK) return r;
        if ((rs->status != 307 && rs->status != 308) || hops >= HTTPS_MAX_REDIRECTS ||
            !https_header(rs, "Location", loc, sizeof(loc)) ||
            !next_url(&u, loc, url, sizeof(url)) || !https_parse_url(url, &to) ||
            !https_redirect_allowed(&u, &to))
            return HTTPS_OK;
        rs->redirects++;
    }
}

/* ---- responses ---- */

void https_response_init(https_response *rs)
{
    memset(rs, 0, sizeof(*rs));
    sb_init(&rs->head);
    sb_init(&rs->body);
}

void https_response_free(https_response *rs)
{
    sb_free(&rs->head);
    sb_free(&rs->body);
}

int https_header(const https_response *rs, const char *name, char *out, size_t n)
{
    const char *p = rs->head.s ? strstr(rs->head.s, "\r\n") : NULL;
    size_t ln = strlen(name);
    while (p && p[2]) {
        const char *line = p + 2, *end = strstr(line, "\r\n");
        if (!end) break;
        if ((size_t)(end - line) > ln && line[ln] == ':' && prefix_ci(line, name)) {
            const char *v = line + ln + 1; size_t vl;
            while (v < end && (*v == ' ' || *v == '\t')) v++;
            vl = (size_t)(end - v);
            while (vl && (v[vl - 1] == ' ' || v[vl - 1] == '\t')) vl--;
            if (vl >= n) vl = n - 1;
            memcpy(out, v, vl); out[vl] = 0;
            return 1;
        }
        p = end;
    }
    if (n) out[0] = 0;
    return 0;
}

const char *https_strerror(int code)
{
    switch (code) {
    case HTTPS_OK:           return "ok";
    case HTTPS_ERR_URL:      return "address not allowed";
    case HTTPS_ERR_TLS:      return "secure connection failed";
    case HTTPS_ERR_NET:      return "network error";
    case HTTPS_ERR_PROTO:    return "the server did not answer in HTTP";
    case HTTPS_ERR_FILE:     return "file not readable";
    case HTTPS_ERR_CANCELED: return "canceled";
    case HTTPS_ERR_TOOBIG:   return "answer too big";
    }
    return "unknown error";
}
