/* fake_https - HTTPS server on 127.0.0.1 for the tests of net.c, tls.c
 * and https.c, with BearSSL on the server side. Built for the host and
 * for XP (make xp-tests), where Python is not there.
 *
 *   fake_https PORTFILE DIR NAME...
 *
 * Each NAME listens on its own port (chosen by the system): it serves the
 * chain DIR/NAME.pem with the key DIR/leaf.key, or plain HTTP for the name
 * "plain". When all are listening PORTFILE gets one "NAME PORT" line each
 * and then "ready". Every connection has its own thread; after two
 * minutes without a new connection the server exits by itself.
 *
 * What it answers, on any port:
 *   /hello          "hello" with Content-Length
 *   /conn           "conn=N req=M": the connection's number and the request's on it
 *   /chunked        50000 bytes in 100 chunks, then a trailer
 *   /close          a body without length, ended by closing the connection
 *   /echo           the request's head as the body
 *   /upload         "len=N sha256=HEX" of the body received; with
 *                   "Expect: 100-continue" a "100 Continue" first
 *   /silent         the same, never answering "100 Continue"
 *   /deny           with Expect: 403 at once, without reading the body
 *   /stall          nothing at all, until the client goes
 *   /drop           "dropped" as if the connection stayed open, then closes it
 *                   (a keep-alive the server ended while the client was idle)
 *   .../services/tasks.php
 *                   Archive.org's tasks API, as "test the keys" asks it:
 *                   200 with a screenname for "Authorization: LOW
 *                   goodaccess:goodsecret", 401 for anything else. With
 *                   /softfail before it, 200 but "success": false; with
 *                   /http500, a server error
 *
 * And a small Archive.org, for the upload tests (core/iaupload.c). The
 * item lives in the folder FAKE_IA_DIR, which the test removes to start
 * from nothing; the fake serves one client at a time, so the files it
 * writes there need no lock.
 *   PUT /s3[-SCENARIO]/ID/NAME
 *                   IA-S3: keeps name, size, MD5 and SHA-1 of the body,
 *                   and the x-archive-meta-* headers of the first PUT.
 *                   Content-MD5 that does not match the body: 400
 *                   BadDigest. Without the good keys: 403
 *   GET  [/ia-SCENARIO]/services/check_identifier.php?identifier=ID
 *   GET  [/ia-SCENARIO]/metadata/ID      the item, or {} if there is none
 *   POST [/ia-SCENARIO]/metadata/ID      a metadata write (the long fields)
 *   GET  [/ia-SCENARIO]/services/tasks.php?identifier=ID&summary=1
 * SCENARIO, in the path, decides what goes wrong: taken (the identifier
 * is not free), slowdown (the first PUT answers 503 SlowDown), spam (503
 * "appears to be spam"), forbidden (403 on every PUT), badmd5 (the item
 * reports another MD5 for the last file), extra (a file under data/ that
 * the package has not), taskerror (a task in error), busy (a task that
 * never ends)
 *   /redirect       307 to /hello;  /redirect-put: 307 to /upload
 *   /redirect-away  307 to https://example.com/hello
 *   /redirect-loop  307 to itself
 *   /quit           "bye", and the server exits */
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <bearssl.h>

#ifdef _WIN32
#include <winsock2.h>
#include <windows.h>
#include <direct.h>
typedef SOCKET sock_t;
typedef int sock_len;
#define sock_close closesocket
#define SEND_FLAGS 0
static LONG volatile conn_count = 0;
#define next_conn() ((int)InterlockedIncrement(&conn_count))
#else
#include <pthread.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/select.h>
#include <sys/socket.h>
typedef int sock_t;
typedef socklen_t sock_len;
#define INVALID_SOCKET (-1)
#define sock_close close
#define SEND_FLAGS MSG_NOSIGNAL
static int volatile conn_count = 0;
#define next_conn() __sync_add_and_fetch(&conn_count, 1)
#endif

#define MAX_PORTS 16
#define IDLE_EXIT_S 120

typedef struct {
    char name[64];
    sock_t ls;
    int port, plain;
    br_x509_certificate chain[4];
    size_t chain_len;
    br_skey_decoder_context key;        /* the key points inside it */
} listener;

typedef struct {
    listener *l;
    sock_t s;
    int no;
    br_ssl_server_context sc;
    br_sslio_context io;
    unsigned char iobuf[BR_SSL_BUFSIZE_BIDI];
    unsigned char rbuf[4096];
    size_t rpos, rlen;
} conn;

typedef struct {
    char method[16], path[512], head[16384];
    long long length;
    int expect;
} request;

/* what a body was: its length and its three digests */
typedef struct { long long len; char sha256[65], md5[33], sha1[41]; } bodyinfo;

#define FAKE_IA_DIR "tmp_unit/fake_ia"

/* ---- files and certificates ---- */

typedef struct { unsigned char *p; size_t len, cap; } bytes;

static void add(void *ctx, const void *d, size_t n)
{
    bytes *b = (bytes *)ctx;
    if (b->len + n > b->cap) {
        b->cap = (b->len + n) * 2;
        b->p = (unsigned char *)realloc(b->p, b->cap);
        if (!b->p) { fprintf(stderr, "fake_https: out of memory\n"); exit(3); }
    }
    memcpy(b->p + b->len, d, n);
    b->len += n;
}

static bytes read_all(const char *path)
{
    bytes b = { NULL, 0, 0 }; unsigned char tmp[4096]; size_t n;
    FILE *f = fopen(path, "rb");
    if (!f) { fprintf(stderr, "fake_https: cannot read %s\n", path); exit(2); }
    while ((n = fread(tmp, 1, sizeof(tmp), f)) > 0) add(&b, tmp, n);
    fclose(f);
    add(&b, "\n", 1);
    return b;
}

/* calls fn with each PEM object of the file */
static void each_pem(const char *path, void (*fn)(void *ctx, const char *name, bytes *der), void *ctx)
{
    bytes f = read_all(path), der = { NULL, 0, 0 };
    br_pem_decoder_context pc; size_t off = 0; char name[128] = "";
    br_pem_decoder_init(&pc);
    while (off < f.len) {
        off += br_pem_decoder_push(&pc, f.p + off, f.len - off);
        switch (br_pem_decoder_event(&pc)) {
        case BR_PEM_BEGIN_OBJ:
            snprintf(name, sizeof(name), "%s", br_pem_decoder_name(&pc));
            der.len = 0;
            br_pem_decoder_setdest(&pc, add, &der);
            break;
        case BR_PEM_END_OBJ: fn(ctx, name, &der); break;
        case BR_PEM_ERROR: fprintf(stderr, "fake_https: bad PEM in %s\n", path); exit(2);
        }
    }
    free(f.p);
}

static void add_cert(void *ctx, const char *name, bytes *der)
{
    listener *l = (listener *)ctx;
    if (strcmp(name, "CERTIFICATE") || l->chain_len >= 4) return;
    l->chain[l->chain_len].data = (unsigned char *)malloc(der->len);
    memcpy(l->chain[l->chain_len].data, der->p, der->len);
    l->chain[l->chain_len++].data_len = der->len;
}

static void add_key(void *ctx, const char *name, bytes *der)
{
    listener *l = (listener *)ctx;
    (void)name;
    br_skey_decoder_init(&l->key);
    br_skey_decoder_push(&l->key, der->p, der->len);
}

static void load_identity(listener *l, const char *dir)
{
    char path[1024];
    snprintf(path, sizeof(path), "%s/%s.pem", dir, l->name);
    each_pem(path, add_cert, l);
    snprintf(path, sizeof(path), "%s/leaf.key", dir);
    each_pem(path, add_key, l);
    if (!l->chain_len || br_skey_decoder_key_type(&l->key) != BR_KEYTYPE_RSA) {
        fprintf(stderr, "fake_https: no certificate or RSA key for %s\n", l->name);
        exit(2);
    }
}

/* ---- connection I/O ---- */

static int sock_read(void *ctx, unsigned char *buf, size_t len)
{
    int r = recv(*(sock_t *)ctx, (char *)buf, (int)len, 0);
    return r > 0 ? r : -1;
}

static int sock_write(void *ctx, const unsigned char *buf, size_t len)
{
    int r = send(*(sock_t *)ctx, (const char *)buf, (int)len, SEND_FLAGS);
    return r > 0 ? r : -1;
}

static int c_read(conn *c, void *buf, size_t n)
{
    if (c->l->plain) return sock_read(&c->s, (unsigned char *)buf, n);
    return br_sslio_read(&c->io, buf, n);
}

static int c_write(conn *c, const void *buf, size_t n)
{
    const char *p = (const char *)buf;
    if (!c->l->plain) return br_sslio_write_all(&c->io, buf, n) == 0 && br_sslio_flush(&c->io) == 0 ? 0 : -1;
    while (n > 0) {
        int r = sock_write(&c->s, (const unsigned char *)p, n);
        if (r < 0) return -1;
        p += r; n -= (size_t)r;
    }
    return 0;
}

static int c_getc(conn *c)
{
    if (c->rpos == c->rlen) {
        int r = c_read(c, c->rbuf, sizeof(c->rbuf));
        if (r <= 0) return -1;
        c->rpos = 0; c->rlen = (size_t)r;
    }
    return c->rbuf[c->rpos++];
}

static int c_line(conn *c, char *out, size_t n)
{
    size_t k = 0; int ch;
    while ((ch = c_getc(c)) >= 0 && ch != '\n') if (ch != '\r' && k + 1 < n) out[k++] = (char)ch;
    out[k] = 0;
    return ch < 0 ? -1 : 0;
}

/* ---- HTTP ---- */

static int read_request(conn *c, request *rq)
{
    char line[2048], proto[16];
    memset(rq, 0, sizeof(*rq));
    if (c_line(c, line, sizeof(line)) < 0) return -1;
    if (sscanf(line, "%15s %511s %15s", rq->method, rq->path, proto) != 3) return -1;
    snprintf(rq->head, sizeof(rq->head), "%s\r\n", line);
    for (;;) {
        size_t hl = strlen(rq->head);
        if (c_line(c, line, sizeof(line)) < 0) return -1;
        if (!line[0]) return 0;
        snprintf(rq->head + hl, sizeof(rq->head) - hl, "%s\r\n", line);
        if (!strncmp(line, "Content-Length:", 15)) rq->length = atoll(line + 15);
        if (!strncmp(line, "Expect: 100-continue", 20)) rq->expect = 1;
    }
}

static int respond(conn *c, int status, const char *extra, const char *body, size_t len)
{
    char head[1024];
    snprintf(head, sizeof(head), "HTTP/1.1 %d %s\r\nContent-Length: %lu\r\n%s\r\n", status,
             status == 200 ? "OK" : status == 307 ? "Temporary Redirect" :
             status == 401 ? "Unauthorized" : status == 403 ? "Forbidden" :
             status == 500 ? "Internal Server Error" : "Not Found",
             (unsigned long)len, extra ? extra : "");
    if (c_write(c, head, strlen(head)) < 0) return -1;
    return len ? c_write(c, body, len) : 0;
}

static void hex(const unsigned char *d, size_t n, char *out)
{
    size_t i;
    for (i = 0; i < n; i++) sprintf(out + 2 * i, "%02x", d[i]);
}

/* reads the body, its three digests in bi and "len=N sha256=HEX" in out */
static int read_body(conn *c, const request *rq, bodyinfo *bi, char *out, size_t n)
{
    br_sha256_context h; br_md5_context m; br_sha1_context s;
    unsigned char d[32], buf[4096]; long long left = rq->length;
    br_sha256_init(&h); br_md5_init(&m); br_sha1_init(&s);
    while (left > 0) {
        size_t k = 0; int ch;
        while (k < sizeof(buf) && (long long)k < left && (ch = c_getc(c)) >= 0) buf[k++] = (unsigned char)ch;
        if (k == 0) return -1;
        br_sha256_update(&h, buf, k);
        br_md5_update(&m, buf, k);
        br_sha1_update(&s, buf, k);
        left -= (long long)k;
    }
    bi->len = rq->length;
    br_sha256_out(&h, d); hex(d, 32, bi->sha256);
    br_md5_out(&m, d);    hex(d, 16, bi->md5);
    br_sha1_out(&s, d);   hex(d, 20, bi->sha1);
    snprintf(out, n, "len=%lld sha256=%s", bi->len, bi->sha256);
    return 0;
}

static int send_chunked(conn *c)
{
    static const char head[] = "HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n";
    char piece[600]; int i;
    if (c_write(c, head, sizeof(head) - 1) < 0) return -1;
    for (i = 0; i < 100; i++) {
        int k = snprintf(piece, sizeof(piece), "1f4;ext=%d\r\n", i);
        memset(piece + k, 'a' + i % 26, 500);
        memcpy(piece + k + 500, "\r\n", 2);
        if (c_write(c, piece, (size_t)k + 502) < 0) return -1;
    }
    return c_write(c, "0\r\nX-Trailer: yes\r\n\r\n", 22);
}

static void stall(conn *c)
{
    while (c_getc(c) >= 0) { }
}

/* Archive.org's tasks API: the keys are in the Authorization header, the
 * only thing this request can fail on */
static int serve_tasks(conn *c, const request *rq)
{
    static const char ok[] = "{\"success\":true,\"value\":{\"summary\":{\"queued\":0,"
                             "\"running\":0},\"screenname\":\"tester\"}}";
    static const char refused[] = "{\"success\":false,\"error\":\"authentication failed\"}";
    static const char soft[] = "{\"success\":false,\"error\":\"no access to this account\"}";
    static const char oops[] = "{\"error\":\"we are having a bad day\"}";
    const char *json = "Content-Type: application/json\r\n";
    if (!strncmp(rq->path, "/softfail/", 10)) return respond(c, 200, json, soft, sizeof(soft) - 1);
    if (!strncmp(rq->path, "/http500/", 9)) return respond(c, 500, json, oops, sizeof(oops) - 1);
    if (strstr(rq->head, "\r\nAuthorization: LOW goodaccess:goodsecret\r\n"))
        return respond(c, 200, json, ok, sizeof(ok) - 1);
    return respond(c, 401, json, refused, sizeof(refused) - 1);
}

/* ---- a small Archive.org ---- */

static void ia_mkdir(void)
{
#ifdef _WIN32
    _mkdir(FAKE_IA_DIR);
#else
    mkdir(FAKE_IA_DIR, 0777);
#endif
}

static void ia_path(const char *name, char *out, size_t n)
{
    snprintf(out, n, "%s/%s", FAKE_IA_DIR, name);
}

/* the scenario the path asks for: "s3-spam/..." or "ia-taken/..." */
static void scenario_of(const char *path, char *out, size_t n)
{
    const char *p = path;
    size_t k = 0;
    out[0] = 0;
    while (*p == '/') p++;
    if (strncmp(p, "s3-", 3) && strncmp(p, "ia-", 3)) return;
    p += 3;
    while (*p && *p != '/' && *p != '?' && k + 1 < n) out[k++] = *p++;
    out[k] = 0;
}

static int is_scenario(const request *rq, const char *want)
{
    char s[32];
    scenario_of(rq->path, s, sizeof(s));
    return !strcmp(s, want);
}

static int good_keys(const request *rq)
{
    return strstr(rq->head, "\r\nAuthorization: LOW goodaccess:goodsecret\r\n") != NULL;
}

/* the value of a request header, "" if absent */
static void req_header(const request *rq, const char *name, char *out, size_t n)
{
    char key[80]; const char *p; size_t k = 0;
    out[0] = 0;
    snprintf(key, sizeof(key), "\r\n%s: ", name);
    if ((p = strstr(rq->head, key)) == NULL) return;
    p += strlen(key);
    while (*p && *p != '\r' && k + 1 < n) out[k++] = *p++;
    out[k] = 0;
}

static int b64_val(char c)
{
    static const char *al = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    const char *p = strchr(al, c);
    return c && p ? (int)(p - al) : -1;
}

/* base64 of 16 bytes (a Content-MD5) as hex; "" if it is not that */
static void b64_md5_hex(const char *s, char *out, size_t n)
{
    unsigned char d[20]; int i, k = 0; unsigned long acc = 0; int bits = 0;
    out[0] = 0;
    for (i = 0; s[i] && s[i] != '='; i++) {
        int v = b64_val(s[i]);
        if (v < 0) return;
        acc = (acc << 6) | (unsigned long)v;
        bits += 6;
        if (bits >= 8) { bits -= 8; if (k < 20) d[k++] = (unsigned char)((acc >> bits) & 0xff); }
    }
    if (k != 16 || n < 33) return;
    hex(d, 16, out);
}

static void json_escaped(const char *s, char *out, size_t n)
{
    size_t k = 0;
    for (; *s && k + 7 < n; s++) {
        if (*s == '"' || *s == '\\') { out[k++] = '\\'; out[k++] = *s; }
        else if (*s == '\n') { out[k++] = '\\'; out[k++] = 'n'; }
        else if (*s == '\r') { out[k++] = '\\'; out[k++] = 'r'; }
        else if (*s == '\t') { out[k++] = '\\'; out[k++] = 't'; }
        else out[k++] = *s;
    }
    out[k] = 0;
}

/* the fields of the first PUT, one "name<TAB>value" line each */
static void ia_store_meta(const request *rq)
{
    char path[256]; const char *p = rq->head; FILE *f;
    ia_path("meta.txt", path, sizeof(path));
    if (!(f = fopen(path, "ab"))) return;
    while ((p = strstr(p, "\r\nx-archive-meta")) != NULL) {
        const char *name, *val, *end;
        char nm[80]; size_t k = 0;
        p += 16;                              /* past "x-archive-meta" */
        if (*p >= '0' && *p <= '9') p += 2;    /* a numbered field */
        if (*p == '-') p++;
        name = p;
        while (*p && *p != ':') p++;
        if (!*p) break;
        while (name < p && k + 1 < sizeof(nm)) nm[k++] = *name++;
        nm[k] = 0;
        val = p + 1;
        while (*val == ' ') val++;
        end = strstr(val, "\r\n");
        if (!end) break;
        fprintf(f, "%s\t%.*s\n", nm, (int)(end - val), val);
        p = end;
    }
    fclose(f);
}

/* PUT /s3[-SCENARIO]/ID/NAME */
static int serve_s3(conn *c, const request *rq, const bodyinfo *bi)
{
    char path[256], id[200], md5[64], want[64], line[1024];
    const char *p = rq->path, *name;
    FILE *f;
    int first;
    size_t k = 0;

    while (*p == '/') p++;
    while (*p && *p != '/') p++;               /* the "s3" segment */
    if (*p == '/') p++;
    while (*p && *p != '/' && k + 1 < sizeof(id)) id[k++] = *p++;
    id[k] = 0;
    name = *p == '/' ? p + 1 : p;

    if (!good_keys(rq) || is_scenario(rq, "forbidden"))
        return respond(c, 403, NULL, "AccessDenied", 12);
    if (is_scenario(rq, "spam")) {
        static const char body[] = "<Error><Code>SlowDown</Code><Message>This item "
                                   "appears to be spam</Message></Error>";
        return respond(c, 503, NULL, body, sizeof(body) - 1);
    }
    ia_mkdir();
    ia_path("puts.txt", path, sizeof(path));
    f = fopen(path, "rb");
    first = f == NULL;
    if (f) fclose(f);
    if (is_scenario(rq, "slowdown") && first) {
        /* the first try finds the queue full; the next one goes through */
        static const char body[] = "<Error><Code>SlowDown</Code><Message>Please reduce "
                                   "your request rate</Message></Error>";
        if ((f = fopen(path, "wb")) != NULL) { fputs("slowed\n", f); fclose(f); }
        return respond(c, 503, NULL, body, sizeof(body) - 1);
    }
    req_header(rq, "Content-MD5", md5, sizeof(md5));
    if (md5[0]) {
        b64_md5_hex(md5, want, sizeof(want));
        if (!want[0] || strcmp(want, bi->md5)) {
            static const char body[] = "<Error><Code>BadDigest</Code></Error>";
            return respond(c, 400, NULL, body, sizeof(body) - 1);
        }
    }
    ia_path("id.txt", path, sizeof(path));
    if ((f = fopen(path, "rb")) == NULL) {
        if ((f = fopen(path, "wb")) != NULL) { fprintf(f, "%s\n", id); fclose(f); }
        ia_store_meta(rq);
    } else {
        fclose(f);
    }
    ia_path("puts.txt", path, sizeof(path));
    if ((f = fopen(path, "ab")) != NULL) { fprintf(f, "%s\n", name); fclose(f); }
    ia_path("files.txt", path, sizeof(path));
    if ((f = fopen(path, "ab")) != NULL) {
        fprintf(f, "%lld %s %s %s\n", bi->len, bi->md5, bi->sha1, name);
        fclose(f);
    }
    snprintf(line, sizeof(line), "stored %s", name);
    return respond(c, 200, NULL, line, strlen(line));
}

static void ia_identifier(char *out, size_t n)
{
    char path[256]; FILE *f;
    out[0] = 0;
    ia_path("id.txt", path, sizeof(path));
    if (!(f = fopen(path, "rb"))) return;
    if (fgets(out, (int)n, f)) {
        char *e = out + strlen(out);
        while (e > out && (e[-1] == '\n' || e[-1] == '\r')) *--e = 0;
    }
    fclose(f);
}

static int serve_ident(conn *c, const request *rq)
{
    char id[200] = "", mine[200]; const char *q = strstr(rq->path, "identifier=");
    char body[300]; int free_id;
    if (q) {
        size_t k = 0;
        q += 11;
        while (*q && *q != '&' && k + 1 < sizeof(id)) id[k++] = *q++;
        id[k] = 0;
    }
    ia_identifier(mine, sizeof(mine));
    free_id = !is_scenario(rq, "taken") && strcmp(id, mine);
    snprintf(body, sizeof(body), "{\"success\":true,\"code\":\"%s\",\"identifier\":\"%s\"}",
             free_id ? "available" : "not_available", id);
    return respond(c, 200, "Content-Type: application/json\r\n", body, strlen(body));
}

static void addf(bytes *b, const char *fmt, ...)
{
    char line[4096]; int n; va_list ap;
    va_start(ap, fmt);
    n = vsnprintf(line, sizeof(line), fmt, ap);
    va_end(ap);
    if (n > 0) add(b, line, (size_t)(n < (int)sizeof(line) ? n : (int)sizeof(line) - 1));
}

#define IA_MAXFIELD 64

/* the fields of meta.txt, those with the same name together in an array */
static void item_metadata(bytes *b, const char *id)
{
    char path[256], line[8192], esc[12000];
    char *name[IA_MAXFIELD], *val[IA_MAXFIELD];
    int n = 0, i, k;
    FILE *f;

    addf(b, "\"metadata\":{\"identifier\":\"%s\"", id);
    ia_path("meta.txt", path, sizeof(path));
    if ((f = fopen(path, "rb")) != NULL) {
        while (n < IA_MAXFIELD && fgets(line, sizeof(line), f)) {
            char *tab, *e = line + strlen(line);
            while (e > line && (e[-1] == '\n' || e[-1] == '\r')) *--e = 0;
            if (!(tab = strchr(line, '\t'))) continue;
            *tab = 0;
            name[n] = strdup(line);
            val[n] = strdup(tab + 1);
            if (!name[n] || !val[n]) break;
            n++;
        }
        fclose(f);
    }
    for (i = 0; i < n; i++) {
        int many = 0, done = 0;
        for (k = 0; k < i; k++) if (!strcmp(name[k], name[i])) done = 1;
        if (done) continue;
        for (k = i + 1; k < n; k++) if (!strcmp(name[k], name[i])) many = 1;
        json_escaped(val[i], esc, sizeof(esc));
        addf(b, many ? ",\"%s\":[\"%s\"" : ",\"%s\":\"%s\"", name[i], esc);
        for (k = i + 1; many && k < n; k++) {
            if (strcmp(name[k], name[i])) continue;
            json_escaped(val[k], esc, sizeof(esc));
            addf(b, ",\"%s\"", esc);
        }
        if (many) addf(b, "]");
    }
    for (i = 0; i < n; i++) { free(name[i]); free(val[i]); }
    addf(b, "}");
}

/* the item as the metadata API reports it */
static int serve_item(conn *c, const request *rq)
{
    char path[256], line[1200], id[200];
    const char *json = "Content-Type: application/json\r\n";
    bytes b = { NULL, 0, 0 };
    FILE *f;
    int rc, n = 0, bad = is_scenario(rq, "badmd5");
    long nfiles = 0;

    ia_identifier(id, sizeof(id));
    if (!id[0]) return respond(c, 200, json, "{}", 2);
    ia_path("files.txt", path, sizeof(path));
    if ((f = fopen(path, "rb")) != NULL) {
        while (fgets(line, sizeof(line), f)) nfiles++;
        fclose(f);
    }
    addf(&b, "{\"created\":1,\"files\":[");
    if ((f = fopen(path, "rb")) != NULL) {
        while (fgets(line, sizeof(line), f)) {
            long long size; char md5[48], sha1[64], name[600]; char *e = line + strlen(line);
            while (e > line && (e[-1] == '\n' || e[-1] == '\r')) *--e = 0;
            if (sscanf(line, "%lld %47s %63s %599[^\n]", &size, md5, sha1, name) != 4) continue;
            n++;
            /* badmd5: the last file comes back with another MD5 */
            if (bad && n == nfiles) md5[0] = md5[0] == '0' ? '1' : '0';
            addf(&b, "%s{\"name\":\"%s\",\"source\":\"original\",\"size\":\"%lld\","
                     "\"md5\":\"%s\",\"sha1\":\"%s\"}", n > 1 ? "," : "", name, size, md5, sha1);
        }
        fclose(f);
    }
    /* the files Archive.org adds itself are not part of the package */
    addf(&b, "%s{\"name\":\"%s_meta.xml\",\"source\":\"metadata\",\"size\":\"120\"}", n ? "," : "", id);
    if (is_scenario(rq, "extra"))
        addf(&b, ",{\"name\":\"data/intruder.bin\",\"source\":\"original\",\"size\":\"7\","
                 "\"md5\":\"d41d8cd98f00b204e9800998ecf8427e\",\"sha1\":\"x\"}");
    addf(&b, "],");
    item_metadata(&b, id);
    addf(&b, "}");
    rc = respond(c, 200, json, (const char *)b.p, b.len);
    free(b.p);
    return rc;
}

/* POST /metadata/ID: a metadata write, the fields a header could not carry */
static int serve_md_write(conn *c, const request *rq)
{
    static const char ok[] = "{\"success\":true,\"task_id\":1,\"log\":\"-\"}";
    const char *json = "Content-Type: application/json\r\n";
    char id[200];
    (void)rq;
    ia_identifier(id, sizeof(id));
    if (!id[0]) {
        static const char no[] = "{\"success\":false,\"error\":\"no such item\"}";
        return respond(c, 404, json, no, sizeof(no) - 1);
    }
    return respond(c, 200, json, ok, sizeof(ok) - 1);
}

/* the tasks of an item: ours are finished, unless the scenario says otherwise */
static int serve_item_tasks(conn *c, const request *rq)
{
    const char *json = "Content-Type: application/json\r\n";
    char body[300];
    int queued = is_scenario(rq, "busy") ? 1 : 0, bad = is_scenario(rq, "taskerror") ? 1 : 0;
    if (!good_keys(rq)) {
        static const char refused[] = "{\"success\":false,\"error\":\"authentication failed\"}";
        return respond(c, 401, json, refused, sizeof(refused) - 1);
    }
    snprintf(body, sizeof(body), "{\"success\":true,\"value\":{\"summary\":{\"queued\":%d,"
                                 "\"running\":0,\"error\":%d,\"paused\":0}}}", queued, bad);
    return respond(c, 200, json, body, strlen(body));
}

/* one request: 0 to go on with the connection, -1 to close it */
static int serve_one(conn *c, int req_no)
{
    request rq; char out[512]; bodyinfo bi;
    if (read_request(c, &rq) < 0) return -1;
    if (!strcmp(rq.path, "/deny") && rq.expect) {
        respond(c, 403, "Connection: close\r\n", "denied", 6);
        return -1;
    }
    if (rq.expect && strcmp(rq.path, "/silent") &&
        c_write(c, "HTTP/1.1 100 Continue\r\n\r\n", 25) < 0) return -1;
    out[0] = 0;
    memset(&bi, 0, sizeof(bi));
    if (rq.length > 0 && read_body(c, &rq, &bi, out, sizeof(out)) < 0) return -1;
    /* Archive.org: the item of the upload tests */
    if (!strncmp(rq.path, "/s3", 3)) return serve_s3(c, &rq, &bi);
    if (strstr(rq.path, "/services/check_identifier.php")) return serve_ident(c, &rq);
    if (strstr(rq.path, "/metadata/"))
        return strcmp(rq.method, "POST") ? serve_item(c, &rq) : serve_md_write(c, &rq);
    if (strstr(rq.path, "/services/tasks.php") && strstr(rq.path, "identifier="))
        return serve_item_tasks(c, &rq);
    if (!strcmp(rq.path, "/hello")) return respond(c, 200, NULL, "hello", 5);
    if (!strcmp(rq.path, "/conn")) {
        snprintf(out, sizeof(out), "conn=%d req=%d", c->no, req_no);
        return respond(c, 200, NULL, out, strlen(out));
    }
    if (!strcmp(rq.path, "/upload") || !strcmp(rq.path, "/silent")) {
        if (!out[0]) snprintf(out, sizeof(out), "len=0");
        return respond(c, 200, NULL, out, strlen(out));
    }
    if (strstr(rq.path, "/services/tasks.php")) return serve_tasks(c, &rq);
    if (!strcmp(rq.path, "/echo")) return respond(c, 200, NULL, rq.head, strlen(rq.head));
    if (!strcmp(rq.path, "/chunked")) return send_chunked(c);
    if (!strcmp(rq.path, "/close")) {
        static const char r[] = "HTTP/1.1 200 OK\r\nConnection: close\r\n\r\nuntil the end";
        c_write(c, r, sizeof(r) - 1);
        return -1;
    }
    if (!strcmp(rq.path, "/stall")) { stall(c); return -1; }
    if (!strcmp(rq.path, "/drop")) { respond(c, 200, NULL, "dropped", 7); return -1; }
    if (!strcmp(rq.path, "/redirect")) return respond(c, 307, "Location: /hello\r\n", NULL, 0);
    if (!strcmp(rq.path, "/redirect-put")) return respond(c, 307, "Location: /upload\r\n", NULL, 0);
    if (!strcmp(rq.path, "/redirect-loop")) return respond(c, 307, "Location: /redirect-loop\r\n", NULL, 0);
    if (!strcmp(rq.path, "/redirect-away"))
        return respond(c, 307, "Location: https://example.com/hello\r\n", NULL, 0);
    if (!strcmp(rq.path, "/quit")) { respond(c, 200, NULL, "bye", 3); exit(0); }
    return respond(c, 404, NULL, "not found", 9);
}

static void serve(conn *c)
{
    int req_no = 1;
    if (!c->l->plain) {
        br_ssl_server_init_full_rsa(&c->sc, c->l->chain, c->l->chain_len,
                                    br_skey_decoder_get_rsa(&c->l->key));
        br_ssl_engine_set_buffer(&c->sc.eng, c->iobuf, sizeof(c->iobuf), 1);
        br_ssl_server_reset(&c->sc);
        br_sslio_init(&c->io, &c->sc.eng, sock_read, &c->s, sock_write, &c->s);
    }
    while (serve_one(c, req_no++) == 0) { }
    if (!c->l->plain) br_sslio_close(&c->io);
    sock_close(c->s);
    free(c);
}

#ifdef _WIN32
static DWORD WINAPI conn_thread(LPVOID p) { serve((conn *)p); return 0; }
static void start(conn *c)
{
    HANDLE h = CreateThread(NULL, 0, conn_thread, c, 0, NULL);
    if (h) CloseHandle(h); else { sock_close(c->s); free(c); }
}
#else
static void *conn_thread(void *p) { serve((conn *)p); return NULL; }
static void start(conn *c)
{
    pthread_t t;
    if (pthread_create(&t, NULL, conn_thread, c) == 0) pthread_detach(t);
    else { sock_close(c->s); free(c); }
}
#endif

/* ---- listening ---- */

static void listen_on(listener *l)
{
    struct sockaddr_in sa; sock_len len = sizeof(sa);
    l->ls = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    memset(&sa, 0, sizeof(sa));
    sa.sin_family = AF_INET;
    sa.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (l->ls == INVALID_SOCKET || bind(l->ls, (struct sockaddr *)&sa, sizeof(sa)) != 0 ||
        listen(l->ls, 16) != 0 || getsockname(l->ls, (struct sockaddr *)&sa, &len) != 0) {
        fprintf(stderr, "fake_https: cannot listen for %s\n", l->name);
        exit(2);
    }
    l->port = ntohs(sa.sin_port);
}

static void write_ports(const char *path, listener *ls, int n)
{
    char tmp[1024]; FILE *f; int i;
    snprintf(tmp, sizeof(tmp), "%s.tmp", path);
    f = fopen(tmp, "w");
    if (!f) { fprintf(stderr, "fake_https: cannot write %s\n", tmp); exit(2); }
    for (i = 0; i < n; i++) fprintf(f, "%s %d\n", ls[i].name, ls[i].port);
    fprintf(f, "ready\n");
    fclose(f);
    remove(path);
    rename(tmp, path);
}

static void accept_loop(listener *ls, int n)
{
    for (;;) {
        fd_set fs; struct timeval tv; int i, maxfd = 0, r;
        FD_ZERO(&fs);
        for (i = 0; i < n; i++) { FD_SET(ls[i].ls, &fs); if ((int)ls[i].ls > maxfd) maxfd = (int)ls[i].ls; }
        tv.tv_sec = IDLE_EXIT_S; tv.tv_usec = 0;
        r = select(maxfd + 1, &fs, NULL, NULL, &tv);
        if (r == 0) exit(0);                 /* forgotten by a test that crashed */
        if (r < 0) continue;
        for (i = 0; i < n; i++) {
            conn *c;
            if (!FD_ISSET(ls[i].ls, &fs)) continue;
            c = (conn *)calloc(1, sizeof(*c));
            if (!c) continue;
            c->l = &ls[i];
            c->s = accept(ls[i].ls, NULL, NULL);
            c->no = next_conn();
            if (c->s == INVALID_SOCKET) { free(c); continue; }
            start(c);
        }
    }
}

int main(int argc, char **argv)
{
    static listener ls[MAX_PORTS];
    int i, n = argc - 3;
#ifdef _WIN32
    WSADATA wd;
    WSAStartup(MAKEWORD(2, 2), &wd);
#endif
    if (n < 1 || n > MAX_PORTS) {
        fprintf(stderr, "usage: fake_https PORTFILE DIR NAME...\n");
        return 2;
    }
    for (i = 0; i < n; i++) {
        snprintf(ls[i].name, sizeof(ls[i].name), "%s", argv[3 + i]);
        ls[i].plain = !strcmp(ls[i].name, "plain");
        if (!ls[i].plain) load_identity(&ls[i], argv[2]);
        listen_on(&ls[i]);
    }
    write_ports(argv[1], ls, n);
    accept_loop(ls, n);
    return 0;
}
