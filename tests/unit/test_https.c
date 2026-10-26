/* net.c, tls.c and https.c against the fake HTTPS server (tests/fake_https.c,
 * certificates in tests/tls), on 127.0.0.1 only: no network needed */
#include "unit.h"
#include "core/https.h"
#include "core/sha256.h"
#include "core/fsu8.h"
#include <stdlib.h>
#include <time.h>

#ifdef _WIN32
#include <windows.h>
#define FAKE_HTTPS "..\\build\\fake_https.exe"
static void ms_sleep(int ms) { Sleep((DWORD)ms); }
#else
#include <pthread.h>
#include <signal.h>
#include <unistd.h>
#include <sys/wait.h>
#define FAKE_HTTPS "../build/fake_https"
static void ms_sleep(int ms) { usleep((useconds_t)ms * 1000); }
#endif

#define PORTS "tmp_unit/https_ports.txt"

/* January 1st of these years, 00:00 UTC */
#define T2001 978307200LL
#define T2002 1009843200LL
#define T2003 1041379200LL
#define T2020 1577836800LL
#define T2100 4102444800LL
#define T2120 4733510400LL
#define T2125 4891363200LL

static const char *const servers[] = {
    "good", "expired", "future", "othername", "unknown", "selfsigned", "plain", NULL
};
static int port[8];

/* ---- the fake server ---- */

#ifdef _WIN32
static HANDLE server_proc;
static int server_start(void)
{
    char cmd[512]; STARTUPINFOA si; PROCESS_INFORMATION pi; int i;
    snprintf(cmd, sizeof(cmd), "%s %s tls", FAKE_HTTPS, PORTS);
    for (i = 0; servers[i]; i++) { strcat(cmd, " "); strcat(cmd, servers[i]); }
    memset(&si, 0, sizeof(si)); si.cb = sizeof(si);
    if (!CreateProcessA(NULL, cmd, NULL, NULL, FALSE, CREATE_NO_WINDOW, NULL, NULL, &si, &pi)) return 0;
    CloseHandle(pi.hThread);
    server_proc = pi.hProcess;
    return 1;
}
static void server_stop(void)
{
    if (!server_proc) return;
    TerminateProcess(server_proc, 0);
    WaitForSingleObject(server_proc, 5000);
    CloseHandle(server_proc);
    server_proc = NULL;
}
#else
static pid_t server_pid;
static int server_start(void)
{
    const char *argv[12]; int i, k = 0;
    argv[k++] = FAKE_HTTPS; argv[k++] = PORTS; argv[k++] = "tls";
    for (i = 0; servers[i]; i++) argv[k++] = servers[i];
    argv[k] = NULL;
    server_pid = fork();
    if (server_pid == 0) { execv(FAKE_HTTPS, (char *const *)argv); _exit(127); }
    return server_pid > 0;
}
static void server_stop(void)
{
    if (server_pid <= 0) return;
    kill(server_pid, SIGTERM);
    waitpid(server_pid, NULL, 0);
    server_pid = 0;
}
#endif

/* the ports from the server's file, once it says "ready" */
static int read_ports(void)
{
    int tries, i;
    for (tries = 0; tries < 100; tries++) {
        FILE *f = fopen(PORTS, "r"); char name[64]; int p, n = 0, ready = 0;
        if (f) {
            while (fscanf(f, "%63s", name) == 1) {
                if (!strcmp(name, "ready")) { ready = 1; break; }
                if (fscanf(f, "%d", &p) != 1) break;
                for (i = 0; servers[i]; i++) if (!strcmp(servers[i], name)) { port[i] = p; n++; }
            }
            fclose(f);
            if (ready) return n == 7;
        }
        ms_sleep(100);
    }
    return 0;
}

static int port_of(const char *name)
{
    int i;
    for (i = 0; servers[i]; i++) if (!strcmp(servers[i], name)) return port[i];
    return 0;
}

static void url_of(char *out, size_t n, const char *server, const char *path)
{
    if (!strcmp(server, "plain")) snprintf(out, n, "http://127.0.0.1:%d%s", port_of(server), path);
    else snprintf(out, n, "https://localhost:%d%s", port_of(server), path);
}

/* ---- helpers ---- */

static tls_anchors *anchors;

static void fixture_hash(const char *path, unsigned char out[32])
{
    char hex[80] = ""; FILE *f = fopen(path, "r"); int i;
    if (f) { if (!fgets(hex, sizeof(hex), f)) hex[0] = 0; fclose(f); }
    for (i = 0; i < 32; i++) { unsigned v = 0; sscanf(hex + 2 * i, "%2x", &v); out[i] = (unsigned char)v; }
}

static tls_config config(void)
{
    tls_config cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.anchors = anchors;
    cfg.connect_ms = 5000;
    cfg.io_ms = 10000;
    return cfg;
}

/* a GET with a new client: the status code or HTTPS_ERR_*; the client
 * (for its tls_info) stays in *keep if not NULL */
static int get(const tls_config *cfg, const char *server, const char *path,
               https_response *rs, https_client **keep)
{
    https_client *c = https_new(cfg); https_request rq; char url[256]; int r;
    memset(&rq, 0, sizeof(rq));
    url_of(url, sizeof(url), server, path);
    rq.method = "GET"; rq.url = url;
    r = https_do(c, &rq, rs);
    if (keep) *keep = c; else https_free(c);
    return r == HTTPS_OK ? rs->status : r;
}

/* ---- pure functions ---- */

static void test_urls(void)
{
    https_url u, v; strbuf b;
    CHECK(https_parse_url("https://s3.us.archive.org/item/data/A.dsk?x=1", &u));
    CHECK(u.tls && u.port == 443);
    CHECK_STR(u.host, "s3.us.archive.org");
    CHECK_STR(u.path, "/item/data/A.dsk?x=1");
    CHECK(https_parse_url("http://127.0.0.1:8080", &u) && !u.tls && u.port == 8080);
    CHECK_STR(u.path, "/");
    CHECK(https_parse_url("https://archive.org?q", &u));
    CHECK_STR(u.path, "/?q");
    CHECK(!https_parse_url("ftp://archive.org/", &u));
    CHECK(!https_parse_url("https://user@archive.org/", &u));
    CHECK(!https_parse_url("https://archive.org/a b", &u));
    CHECK(!https_parse_url("https://archive.org/a\r\nX: y", &u));
    CHECK(!https_parse_url("https://archive.org:99999/", &u));

    /* plain http: only 127.0.0.1, and only in the test build */
    CHECK(https_test_mode());
    CHECK(https_parse_url("http://127.0.0.1:1/", &u) && https_url_allowed(&u));
    CHECK(https_parse_url("http://localhost:1/", &u) && !https_url_allowed(&u));
    CHECK(https_parse_url("http://archive.org/", &u) && !https_url_allowed(&u));

    /* redirects: only to https archive.org, or the fake server to itself */
    https_parse_url("https://s3.us.archive.org/x", &u);
    CHECK(https_parse_url("https://ia800.us.archive.org/x", &v) && https_redirect_allowed(&u, &v));
    CHECK(https_parse_url("https://archive.org/x", &v) && https_redirect_allowed(&u, &v));
    CHECK(https_parse_url("http://ia800.us.archive.org/x", &v) && !https_redirect_allowed(&u, &v));
    CHECK(https_parse_url("https://evilarchive.org/x", &v) && !https_redirect_allowed(&u, &v));
    CHECK(https_parse_url("https://archive.org.evil.com/x", &v) && !https_redirect_allowed(&u, &v));
    https_parse_url("https://localhost:5000/a", &u);
    CHECK(https_parse_url("https://localhost:5000/b", &v) && https_redirect_allowed(&u, &v));
    CHECK(https_parse_url("https://localhost:5001/b", &v) && !https_redirect_allowed(&u, &v));

    sb_init(&b);
    https_pct(&b, "Caf\xc3\xa8 & co/x~y", NULL);
    CHECK_STR(b.s, "Caf%C3%A8%20%26%20co%2Fx~y");
    sb_clear(&b);
    https_pct(&b, "/data/FLOPPY 01.dsk", "/");
    CHECK_STR(b.s, "/data/FLOPPY%2001.dsk");
    sb_clear(&b);
    https_form(&b, "-target", "metadata");
    https_form(&b, "-patch", "[{\"op\":\"add\",\"path\":\"/title\",\"value\":\"a=b&c\"}]");
    CHECK_STR(b.s, "-target=metadata&-patch=%5B%7B%22op%22%3A%22add%22%2C%22path%22%3A%22%2Ftitle"
                   "%22%2C%22value%22%3A%22a%3Db%26c%22%7D%5D");
    sb_free(&b);
}

static void test_anchors(void)
{
    int n = -1; tls_anchors *a;
    a = tls_anchors_load("tls/ca.pem", &n);
    CHECK(a != NULL && n == 1);
    tls_anchors_free(a);
    /* the good chain holds two certificates: both become anchors */
    a = tls_anchors_load("tls/good.pem", &n);
    CHECK(a != NULL && n == 2);
    tls_anchors_free(a);
    CHECK(tls_anchors_load("tls/missing.pem", &n) == NULL && n == 0);
    CHECK(tls_anchors_load("tls/good.sha256", &n) == NULL && n == 0);
    CHECK(tls_anchors_load("tls/leaf.key", &n) == NULL);     /* no certificate in it */
}

/* ---- requests ---- */

static void test_get(void)
{
    tls_config cfg = config(); https_response rs; https_client *c = NULL;
    unsigned char fp[32]; char v[64];
    https_response_init(&rs);
    fixture_hash("tls/good.sha256", fp);

    CHECK(get(&cfg, "good", "/hello", &rs, &c) == 200);
    CHECK_STR(rs.body.s, "hello");
    CHECK(https_header(&rs, "content-length", v, sizeof(v)) && !strcmp(v, "5"));
    CHECK(!https_header(&rs, "Content-Type", v, sizeof(v)));
    if (c) {
        const tls_info *ti = https_tls_info(c);
        CHECK(ti->why == TLS_OK && ti->have_cert);
        CHECK(!memcmp(ti->cert_sha256, fp, 32));      /* the fingerprint seen, also with anchors */
        CHECK(ti->not_before == T2020);
        https_free(c);
    }

    CHECK(get(&cfg, "good", "/chunked", &rs, NULL) == 200);
    CHECK(rs.body.len == 50000);
    CHECK(rs.body.len == 50000 && rs.body.s[0] == 'a' && rs.body.s[499] == 'a' &&
          rs.body.s[500] == 'b' && rs.body.s[49999] == 'a' + 99 % 26);

    CHECK(get(&cfg, "good", "/close", &rs, NULL) == 200);
    CHECK_STR(rs.body.s, "until the end");

    CHECK(get(&cfg, "good", "/nothing", &rs, NULL) == 404);
    CHECK_STR(rs.body.s, "not found");

    CHECK(get(&cfg, "plain", "/hello", &rs, NULL) == 200);
    CHECK_STR(rs.body.s, "hello");
    https_response_free(&rs);
}

/* the second request goes on the same connection; after a body ended by
 * the close, a new one */
static void test_keepalive(void)
{
    tls_config cfg = config(); https_client *c = https_new(&cfg);
    https_request rq; https_response rs; char url[256], first[64];
    memset(&rq, 0, sizeof(rq));
    https_response_init(&rs);
    url_of(url, sizeof(url), "good", "/conn");
    rq.method = "GET"; rq.url = url;
    CHECK(https_do(c, &rq, &rs) == HTTPS_OK && !rs.reused);
    str_copy(first, sizeof(first), rs.body.s);
    CHECK(!strncmp(first, "conn=", 5) && strstr(first, " req=1"));
    CHECK(https_do(c, &rq, &rs) == HTTPS_OK && rs.reused);
    CHECK(strstr(rs.body.s, " req=2") && !strncmp(rs.body.s, first, strcspn(first, " ")));

    url_of(url, sizeof(url), "good", "/close");
    CHECK(https_do(c, &rq, &rs) == HTTPS_OK && rs.reused);
    url_of(url, sizeof(url), "good", "/conn");
    CHECK(https_do(c, &rq, &rs) == HTTPS_OK && !rs.reused && strstr(rs.body.s, " req=1"));

    /* the server closed the idle connection: the request goes again on a new one */
    url_of(url, sizeof(url), "good", "/drop");
    CHECK(https_do(c, &rq, &rs) == HTTPS_OK && rs.reused);
    CHECK_STR(rs.body.s, "dropped");
    ms_sleep(200);
    url_of(url, sizeof(url), "good", "/conn");
    CHECK(https_do(c, &rq, &rs) == HTTPS_OK && rs.status == 200 && !rs.reused);
    CHECK(strstr(rs.body.s, " req=1") != NULL);
    url_of(url, sizeof(url), "plain", "/drop");
    CHECK(https_do(c, &rq, &rs) == HTTPS_OK);
    ms_sleep(200);
    url_of(url, sizeof(url), "plain", "/conn");
    CHECK(https_do(c, &rq, &rs) == HTTPS_OK && rs.status == 200 && strstr(rs.body.s, " req=1"));

    /* the extra headers reach the server, Host with the port */
    url_of(url, sizeof(url), "good", "/echo");
    rq.headers = "X-Test: one\r\nAuthorization: LOW a:b\r\n";
    CHECK(https_do(c, &rq, &rs) == HTTPS_OK && rs.status == 200);
    CHECK(strstr(rs.body.s, "GET /echo HTTP/1.1\r\n") && strstr(rs.body.s, "X-Test: one\r\n"));
    CHECK(strstr(rs.body.s, "\r\nHost: localhost:") && !strstr(rs.body.s, "Content-Length"));
    https_response_free(&rs);
    https_free(c);
}

typedef struct { sha256_ctx h; long long bytes, next; int calls, bad_off, stop_at; } body_seen;

static int on_body(void *ctx, long long off, const void *data, size_t n)
{
    body_seen *b = (body_seen *)ctx;
    if (off == 0) { sha256_init(&b->h); b->bytes = 0; b->next = 0; }
    if (off != b->next) b->bad_off = 1;
    sha256_update(&b->h, data, n);
    b->bytes += (long long)n; b->next = off + (long long)n; b->calls++;
    return b->stop_at && b->calls >= b->stop_at;
}

/* the file the uploads send: 300000 bytes that are not all the same */
static void make_upload(const char *path)
{
    FILE *f = fopen(path, "wb"); long i;
    if (!f) return;
    for (i = 0; i < 300000; i++) fputc((int)((i * 7 + i / 251) & 0xff), f);
    fclose(f);
}

static void expect_digest(const https_response *rs, const char *path, long long off, long long len)
{
    FILE *f = fopen(path, "rb"); sha256_ctx h; unsigned char d[32], buf[4096]; char hex[65], want[128];
    long long left = len; size_t n;
    if (!f) { CHECK(f != NULL); return; }
    fseek(f, (long)off, SEEK_SET);
    sha256_init(&h);
    while (left > 0 && (n = fread(buf, 1, left > 4096 ? 4096 : (size_t)left, f)) > 0) {
        sha256_update(&h, buf, n); left -= (long long)n;
    }
    fclose(f);
    sha256_final(&h, d);
    sha256_hex(d, hex);
    snprintf(want, sizeof(want), "len=%lld sha256=%s", len, hex);
    CHECK_STR(rs->body.s, want);
}

static void check_seen(body_seen *b, long long len)
{
    unsigned char d[32];
    CHECK(!b->bad_off && b->bytes == len);
    CHECK(b->calls == (int)((len + HTTPS_PIECE - 1) / HTTPS_PIECE));
    sha256_final(&b->h, d);
}

static void test_upload(void)
{
    tls_config cfg = config(); https_client *c = https_new(&cfg);
    https_request rq; https_response rs; body_seen seen; char url[256];
    const char *file = "tmp_unit/upload.bin";
    time_t t0;
    u8_mkdir("tmp_unit");
    make_upload(file);
    https_response_init(&rs);
    memset(&rq, 0, sizeof(rq));
    memset(&seen, 0, sizeof(seen));
    url_of(url, sizeof(url), "good", "/upload");
    rq.method = "PUT"; rq.url = url; rq.body_file = file; rq.file_len = -1;
    rq.on_body = on_body; rq.ctx = &seen;

    CHECK(https_do(c, &rq, &rs) == HTTPS_OK && rs.status == 200);
    expect_digest(&rs, file, 0, 300000);
    check_seen(&seen, 300000);

    /* a part of the file (a multipart part), with Expect: 100-continue */
    memset(&seen, 0, sizeof(seen));
    rq.file_offset = 70000; rq.file_len = 100000; rq.expect_continue = 1;
    CHECK(https_do(c, &rq, &rs) == HTTPS_OK && rs.status == 200 && rs.reused);
    expect_digest(&rs, file, 70000, 100000);
    check_seen(&seen, 100000);

    /* refused before the body: the body does not go */
    memset(&seen, 0, sizeof(seen));
    url_of(url, sizeof(url), "good", "/deny");
    CHECK(https_do(c, &rq, &rs) == HTTPS_OK && rs.status == 403 && seen.calls == 0);
    CHECK_STR(rs.body.s, "denied");

    /* a server that never says "100 Continue": the body goes after 3 s */
    memset(&seen, 0, sizeof(seen));
    url_of(url, sizeof(url), "good", "/silent");
    t0 = time(NULL);
    CHECK(https_do(c, &rq, &rs) == HTTPS_OK && rs.status == 200);
    CHECK(time(NULL) - t0 >= 2 && time(NULL) - t0 <= 6);
    expect_digest(&rs, file, 70000, 100000);

    /* from memory, and an empty PUT */
    rq.body_file = NULL; rq.body = "abc"; rq.body_len = 3; rq.expect_continue = 0;
    rq.on_body = NULL;
    url_of(url, sizeof(url), "plain", "/upload");
    CHECK(https_do(c, &rq, &rs) == HTTPS_OK && rs.status == 200);
    CHECK_STR(rs.body.s, "len=3 sha256=ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
    rq.body = NULL; rq.body_len = 0;
    CHECK(https_do(c, &rq, &rs) == HTTPS_OK && rs.status == 200);
    CHECK_STR(rs.body.s, "len=0");

    /* the callback stops the upload; the client goes on with a new connection */
    memset(&seen, 0, sizeof(seen));
    seen.stop_at = 2;
    rq.body_file = file; rq.file_offset = 0; rq.file_len = -1; rq.on_body = on_body;
    url_of(url, sizeof(url), "good", "/upload");
    CHECK(https_do(c, &rq, &rs) == HTTPS_ERR_CANCELED && seen.calls == 2);
    seen.stop_at = 0;
    CHECK(https_do(c, &rq, &rs) == HTTPS_OK && rs.status == 200 && !rs.reused);

    /* a missing file, a part beyond the end */
    rq.body_file = "tmp_unit/missing.bin";
    CHECK(https_do(c, &rq, &rs) == HTTPS_ERR_FILE);
    rq.body_file = file; rq.file_offset = 299000; rq.file_len = 2000;
    CHECK(https_do(c, &rq, &rs) == HTTPS_ERR_FILE);
    https_response_free(&rs);
    https_free(c);
}

static void test_redirects(void)
{
    tls_config cfg = config(); https_client *c = https_new(&cfg);
    https_request rq; https_response rs; body_seen seen; char url[256];
    https_response_init(&rs);
    memset(&rq, 0, sizeof(rq));
    rq.method = "GET"; rq.url = url;
    url_of(url, sizeof(url), "good", "/redirect");
    CHECK(https_do(c, &rq, &rs) == HTTPS_OK && rs.status == 200 && rs.redirects == 1);
    CHECK_STR(rs.body.s, "hello");
    url_of(url, sizeof(url), "good", "/redirect-away");
    CHECK(https_do(c, &rq, &rs) == HTTPS_OK && rs.status == 307 && rs.redirects == 0);
    url_of(url, sizeof(url), "good", "/redirect-loop");
    CHECK(https_do(c, &rq, &rs) == HTTPS_OK && rs.status == 307 && rs.redirects == HTTPS_MAX_REDIRECTS);

    /* 307 keeps method and body: the body is sent again from 0 */
    memset(&seen, 0, sizeof(seen));
    rq.method = "PUT"; rq.body = "abc"; rq.body_len = 3; rq.on_body = on_body; rq.ctx = &seen;
    url_of(url, sizeof(url), "good", "/redirect-put");
    CHECK(https_do(c, &rq, &rs) == HTTPS_OK && rs.status == 200 && rs.redirects == 1);
    CHECK(seen.calls == 2 && seen.bytes == 3 && !seen.bad_off);
    CHECK(!strncmp(rs.body.s, "len=3 ", 6));
    https_response_free(&rs);
    https_free(c);
}

/* ---- trust ---- */

static void test_trust(void)
{
    tls_config cfg = config(); https_response rs; https_client *c = NULL;
    unsigned char self[32], good[32];
    https_response_init(&rs);
    fixture_hash("tls/selfsigned.sha256", self);
    fixture_hash("tls/good.sha256", good);

#define TRY(server, want_why) do { \
        CHECK(get(&cfg, server, "/hello", &rs, &c) == HTTPS_ERR_TLS); \
        CHECK(c && https_tls_info(c)->why == (want_why)); } while (0)

    CHECK(tls_release_time() > T2020 && tls_release_time() % 86400 == 0);
    TRY("unknown", TLS_ERR_UNKNOWN_ROOT);
    https_free(c);
    TRY("othername", TLS_ERR_BAD_NAME);
    https_free(c);
    /* BearSSL checks the name first: nas.test is not localhost */
    TRY("selfsigned", TLS_ERR_BAD_NAME);
    https_free(c);

    /* the clock: the dates of the refused certificate, for the message */
    TRY("expired", TLS_ERR_CLOCK);
    CHECK(c && https_tls_info(c)->not_before == T2001 && https_tls_info(c)->not_after == T2002);
    CHECK(c && https_tls_info(c)->now > T2020);
    https_free(c);
    TRY("future", TLS_ERR_CLOCK);
    CHECK(c && https_tls_info(c)->not_before == T2100);
    https_free(c);
    /* a PC that says 2003, without touching its clock: before the release,
     * refused without even connecting */
    cfg.now = T2003;
    TRY("good", TLS_ERR_CLOCK);
    CHECK(c && https_tls_info(c)->now == T2003 && !https_tls_info(c)->have_cert);
    CHECK(c && https_tls_info(c)->not_before == tls_release_time() && https_tls_info(c)->not_after == 0);
    https_free(c);
    /* one that says 2125: the certificates refuse it */
    cfg.now = T2125;
    TRY("good", TLS_ERR_CLOCK);
    CHECK(c && https_tls_info(c)->have_cert && https_tls_info(c)->not_after == T2120);
    https_free(c);
    cfg.now = 0;

    /* the pin: the self-signed certificate of a NAS, name and root ignored */
    cfg.anchors = NULL;
    cfg.pin = self;
    CHECK(get(&cfg, "selfsigned", "/hello", &rs, &c) == 200);
    CHECK_STR(rs.body.s, "hello");
    CHECK(c && !memcmp(https_tls_info(c)->cert_sha256, self, 32));
    https_free(c);
    cfg.pin = good;
    TRY("selfsigned", TLS_ERR_PIN);
    CHECK(c && https_tls_info(c)->have_cert && !memcmp(https_tls_info(c)->cert_sha256, self, 32));
    https_free(c);
    CHECK(get(&cfg, "good", "/hello", &rs, NULL) == 200);
    cfg.pin = NULL;
    TRY("good", TLS_ERR_CONFIG);
    https_free(c);
#undef TRY
    https_response_free(&rs);
}

/* ---- timeouts and cancel ---- */

static https_client *to_cancel;

#ifdef _WIN32
static DWORD WINAPI canceler(LPVOID p) { (void)p; Sleep(400); https_cancel(to_cancel); return 0; }
#else
static void *canceler(void *p) { (void)p; ms_sleep(400); https_cancel(to_cancel); return NULL; }
#endif

static void cancel_later(void)
{
#ifdef _WIN32
    HANDLE h = CreateThread(NULL, 0, canceler, NULL, 0, NULL);
    if (h) CloseHandle(h);
#else
    pthread_t t;
    if (pthread_create(&t, NULL, canceler, NULL) == 0) pthread_detach(t);
#endif
}

static void test_timeouts(void)
{
    tls_config cfg = config(); https_client *c; https_response rs; time_t t0; char url[64];
    https_request rq;
    https_response_init(&rs);

    cfg.io_ms = 800;
    t0 = time(NULL);
    CHECK(get(&cfg, "good", "/stall", &rs, &c) == HTTPS_ERR_NET);
    CHECK(c && https_net_error(c) == NET_ERR_TIMEOUT);
    CHECK(time(NULL) - t0 <= 4);
    https_free(c);

    /* nobody listening */
    cfg.connect_ms = 3000;
    memset(&rq, 0, sizeof(rq));
    rq.method = "GET"; rq.url = url;
    snprintf(url, sizeof(url), "https://127.0.0.1:1/");
    c = https_new(&cfg);
    CHECK(https_do(c, &rq, &rs) == HTTPS_ERR_NET && https_net_error(c) == NET_ERR_CONNECT);
    https_free(c);

    /* Interrompi: from another thread, during a long wait */
    cfg.io_ms = 30000;
    c = to_cancel = https_new(&cfg);
    url_of(url, sizeof(url), "good", "/stall");
    t0 = time(NULL);
    cancel_later();
    CHECK(https_do(c, &rq, &rs) == HTTPS_ERR_CANCELED);
    CHECK(time(NULL) - t0 <= 4);
    url_of(url, sizeof(url), "good", "/hello");
    CHECK(https_do(c, &rq, &rs) == HTTPS_ERR_CANCELED);     /* it stays until cleared */
    https_uncancel(c);
    CHECK(https_do(c, &rq, &rs) == HTTPS_OK && rs.status == 200);
    https_free(c);
    https_response_free(&rs);
}

void test_https(void)
{
    remove(PORTS);
    u8_mkdir("tmp_unit");
    test_urls();
    test_anchors();
    if (!server_start() || !read_ports()) {
        CHECK(!"fake_https did not start");
        server_stop();
        return;
    }
    anchors = tls_anchors_load("tls/ca.pem", NULL);
    test_get();
    test_keepalive();
    test_upload();
    test_redirects();
    test_trust();
    test_timeouts();
    tls_anchors_free(anchors);
    server_stop();
}
