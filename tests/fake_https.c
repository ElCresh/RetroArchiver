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
 *   /redirect       307 to /hello;  /redirect-put: 307 to /upload
 *   /redirect-away  307 to https://example.com/hello
 *   /redirect-loop  307 to itself
 *   /quit           "bye", and the server exits */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <bearssl.h>

#ifdef _WIN32
#include <winsock2.h>
#include <windows.h>
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
    char method[16], path[256], head[8192];
    long long length;
    int expect;
} request;

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
    if (sscanf(line, "%15s %255s %15s", rq->method, rq->path, proto) != 3) return -1;
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
             status == 403 ? "Forbidden" : "Not Found",
             (unsigned long)len, extra ? extra : "");
    if (c_write(c, head, strlen(head)) < 0) return -1;
    return len ? c_write(c, body, len) : 0;
}

static void hex(const unsigned char *d, size_t n, char *out)
{
    size_t i;
    for (i = 0; i < n; i++) sprintf(out + 2 * i, "%02x", d[i]);
}

/* reads the body and leaves "len=N sha256=HEX" in out */
static int read_body(conn *c, const request *rq, char *out, size_t n)
{
    br_sha256_context h; unsigned char d[32], buf[4096]; char hx[65]; long long left = rq->length;
    br_sha256_init(&h);
    while (left > 0) {
        size_t k = 0; int ch;
        while (k < sizeof(buf) && (long long)k < left && (ch = c_getc(c)) >= 0) buf[k++] = (unsigned char)ch;
        if (k == 0) return -1;
        br_sha256_update(&h, buf, k);
        left -= (long long)k;
    }
    br_sha256_out(&h, d);
    hex(d, 32, hx);
    snprintf(out, n, "len=%lld sha256=%s", rq->length, hx);
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

/* one request: 0 to go on with the connection, -1 to close it */
static int serve_one(conn *c, int req_no)
{
    request rq; char out[512];
    if (read_request(c, &rq) < 0) return -1;
    if (!strcmp(rq.path, "/deny") && rq.expect) {
        respond(c, 403, "Connection: close\r\n", "denied", 6);
        return -1;
    }
    if (rq.expect && strcmp(rq.path, "/silent") &&
        c_write(c, "HTTP/1.1 100 Continue\r\n\r\n", 25) < 0) return -1;
    out[0] = 0;
    if (rq.length > 0 && read_body(c, &rq, out, sizeof(out)) < 0) return -1;
    if (!strcmp(rq.path, "/hello")) return respond(c, 200, NULL, "hello", 5);
    if (!strcmp(rq.path, "/conn")) {
        snprintf(out, sizeof(out), "conn=%d req=%d", c->no, req_no);
        return respond(c, 200, NULL, out, strlen(out));
    }
    if (!strcmp(rq.path, "/upload") || !strcmp(rq.path, "/silent")) {
        if (!out[0]) snprintf(out, sizeof(out), "len=0");
        return respond(c, 200, NULL, out, strlen(out));
    }
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
