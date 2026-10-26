#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <bearssl.h>
#include "tls.h"
#include "fsu8.h"
#include "version.h"

#ifdef _WIN32
#include <windows.h>
#endif

/* a CA bundle is a few hundred KB: anything far bigger is not one */
#define PEM_MAX (16L * 1024 * 1024)

/* Unix Epoch in BearSSL's days since January 1st, 0 AD */
#define UNIX_EPOCH_DAYS 719528

/* pump: the target state was reached / nothing came within the wait */
#define PUMP_READY   1
#define PUMP_NOTHING 0

typedef struct { unsigned char *p; size_t len, cap; } bytes;

struct tls_anchors { br_x509_trust_anchor *ta; size_t n; };

/* The X.509 engine BearSSL calls: it forwards to br_x509_minimal (anchors)
 * or checks the pin, and in both cases hashes the server certificate */
typedef struct {
    const br_x509_class *vtable;
    br_x509_minimal_context minimal;
    const unsigned char *pin;            /* NULL: the minimal engine decides */
    int certs;                           /* certificates of the chain started */
    br_sha256_context fp;
    unsigned char seen[32];
    int have_seen, pin_bad;
    br_x509_decoder_context dec;         /* pin: the server certificate's key */
} x509_wrap;

struct tls_conn {
    br_ssl_client_context cc;
    x509_wrap x5;
    unsigned char iobuf[BR_SSL_BUFSIZE_BIDI];
    net_conn *net;
    tls_config cfg;
    int open, net_err, last_err;
    long long now, not_before, not_after;
    int dates_seen, dates_failed;
};

/* ---- the trust anchors ---- */

static void bytes_add(void *ctx, const void *data, size_t n)
{
    bytes *b = (bytes *)ctx;
    if (b->len + n > b->cap) {
        size_t cap = b->cap ? b->cap * 2 : 1024; unsigned char *p;
        while (cap < b->len + n) cap *= 2;
        p = (unsigned char *)realloc(b->p, cap);
        if (!p) return;                  /* the object comes out short and does not decode */
        b->p = p; b->cap = cap;
    }
    memcpy(b->p + b->len, data, n);
    b->len += n;
}

static unsigned char *dup_bytes(const unsigned char *p, size_t n)
{
    unsigned char *d = (unsigned char *)malloc(n ? n : 1);
    if (d) memcpy(d, p, n);
    return d;
}

static int copy_pkey(br_x509_pkey *dst, const br_x509_pkey *src)
{
    *dst = *src;
    if (src->key_type == BR_KEYTYPE_RSA) {
        dst->key.rsa.n = dup_bytes(src->key.rsa.n, src->key.rsa.nlen);
        dst->key.rsa.e = dup_bytes(src->key.rsa.e, src->key.rsa.elen);
        return dst->key.rsa.n && dst->key.rsa.e;
    }
    dst->key.ec.q = dup_bytes(src->key.ec.q, src->key.ec.qlen);
    return dst->key.ec.q != NULL;
}

static void free_anchor(br_x509_trust_anchor *ta)
{
    free(ta->dn.data);
    if (ta->pkey.key_type == BR_KEYTYPE_RSA) { free(ta->pkey.key.rsa.n); free(ta->pkey.key.rsa.e); }
    else free(ta->pkey.key.ec.q);
}

/* one certificate of the bundle becomes an anchor; 0 if it does not decode */
static int add_anchor(tls_anchors *a, const bytes *der)
{
    br_x509_decoder_context dc; bytes dn = { NULL, 0, 0 };
    const br_x509_pkey *pk; br_x509_trust_anchor *ta, *grown;
    br_x509_decoder_init(&dc, bytes_add, &dn);
    br_x509_decoder_push(&dc, der->p, der->len);
    pk = br_x509_decoder_get_pkey(&dc);
    grown = pk ? (br_x509_trust_anchor *)realloc(a->ta, (a->n + 1) * sizeof(*a->ta)) : NULL;
    if (!grown) { free(dn.p); return 0; }
    a->ta = grown;
    ta = &a->ta[a->n];
    memset(ta, 0, sizeof(*ta));
    ta->dn.data = dn.p; ta->dn.len = dn.len;
    ta->flags = br_x509_decoder_isCA(&dc) ? BR_X509_TA_CA : 0;
    if (!copy_pkey(&ta->pkey, pk)) { free_anchor(ta); return 0; }
    a->n++;
    return 1;
}

static int is_cert_name(const char *name)
{
    return !strcmp(name, "CERTIFICATE") || !strcmp(name, "X509 CERTIFICATE");
}

/* the PEM objects of buf: the certificates become anchors, the rest is skipped */
static int decode_pem(tls_anchors *a, const unsigned char *buf, size_t len)
{
    br_pem_decoder_context pc; bytes der = { NULL, 0, 0 }; int inobj = 0, ok = 1;
    br_pem_decoder_init(&pc);
    while (len > 0 && ok) {
        size_t used = br_pem_decoder_push(&pc, buf, len);
        buf += used; len -= used;
        switch (br_pem_decoder_event(&pc)) {
        case BR_PEM_BEGIN_OBJ:
            inobj = is_cert_name(br_pem_decoder_name(&pc));
            der.len = 0;
            br_pem_decoder_setdest(&pc, inobj ? bytes_add : NULL, &der);
            break;
        case BR_PEM_END_OBJ:
            if (inobj) add_anchor(a, &der);
            inobj = 0;
            break;
        case BR_PEM_ERROR:
            ok = 0;
            break;
        }
    }
    free(der.p);
    return ok;
}

static unsigned char *read_file(const char *path, size_t *len)
{
    FILE *f = u8_fopen(path, "rb"); bytes b = { NULL, 0, 0 }; unsigned char tmp[16384]; size_t n;
    if (!f) return NULL;
    while ((n = fread(tmp, 1, sizeof(tmp), f)) > 0 && b.len < (size_t)PEM_MAX) bytes_add(&b, tmp, n);
    fclose(f);
    /* the PEM decoder sees the last line only after its newline */
    bytes_add(&b, "\n", 1);
    *len = b.len;
    return b.p;
}

tls_anchors *tls_anchors_load(const char *path, int *count)
{
    size_t len = 0; unsigned char *buf = read_file(path, &len);
    tls_anchors *a = buf ? (tls_anchors *)calloc(1, sizeof(*a)) : NULL;
    if (count) *count = 0;
    if (a && (!decode_pem(a, buf, len) || a->n == 0)) { tls_anchors_free(a); a = NULL; }
    free(buf);
    if (a && count) *count = (int)a->n;
    return a;
}

void tls_anchors_free(tls_anchors *a)
{
    size_t i;
    if (!a) return;
    for (i = 0; i < a->n; i++) free_anchor(&a->ta[i]);
    free(a->ta);
    free(a);
}

/* ---- the X.509 engine ---- */

static x509_wrap *wrap_of(const br_x509_class *const *ctx)
{
    return (x509_wrap *)(void *)ctx;
}

static void xw_start_chain(const br_x509_class **ctx, const char *server_name)
{
    x509_wrap *w = wrap_of(ctx);
    w->certs = 0; w->have_seen = 0; w->pin_bad = 0;
    if (!w->pin) w->minimal.vtable->start_chain(&w->minimal.vtable, server_name);
}

static void xw_start_cert(const br_x509_class **ctx, uint32_t length)
{
    x509_wrap *w = wrap_of(ctx);
    if (w->certs++ == 0) {
        br_sha256_init(&w->fp);
        if (w->pin) br_x509_decoder_init(&w->dec, NULL, NULL);
    }
    if (!w->pin) w->minimal.vtable->start_cert(&w->minimal.vtable, length);
}

static void xw_append(const br_x509_class **ctx, const unsigned char *buf, size_t len)
{
    x509_wrap *w = wrap_of(ctx);
    if (w->certs == 1) {
        br_sha256_update(&w->fp, buf, len);
        if (w->pin) br_x509_decoder_push(&w->dec, buf, len);
    }
    if (!w->pin) w->minimal.vtable->append(&w->minimal.vtable, buf, len);
}

static void xw_end_cert(const br_x509_class **ctx)
{
    x509_wrap *w = wrap_of(ctx);
    if (w->certs == 1) { br_sha256_out(&w->fp, w->seen); w->have_seen = 1; }
    if (!w->pin) w->minimal.vtable->end_cert(&w->minimal.vtable);
}

/* the pin: the server certificate must be exactly the expected one; its
 * dates and names do not count (a NAS certificate the user confirmed) */
static unsigned xw_end_chain(const br_x509_class **ctx)
{
    x509_wrap *w = wrap_of(ctx);
    int err;
    if (!w->pin) return w->minimal.vtable->end_chain(&w->minimal.vtable);
    if (!w->have_seen) return BR_ERR_X509_EMPTY_CHAIN;
    if (memcmp(w->seen, w->pin, 32) != 0) { w->pin_bad = 1; return BR_ERR_X509_NOT_TRUSTED; }
    err = br_x509_decoder_last_error(&w->dec);
    if (err) return (unsigned)err;
    return br_x509_decoder_get_pkey(&w->dec) ? 0 : BR_ERR_X509_UNSUPPORTED;
}

static const br_x509_pkey *xw_get_pkey(const br_x509_class *const *ctx, unsigned *usages)
{
    x509_wrap *w = wrap_of(ctx);
    if (!w->pin) return w->minimal.vtable->get_pkey(&w->minimal.vtable, usages);
    if (usages) *usages = BR_KEYTYPE_KEYX | BR_KEYTYPE_SIGN;
    return br_x509_decoder_get_pkey(&w->dec);
}

static const br_x509_class xw_vtable = {
    sizeof(x509_wrap), xw_start_chain, xw_start_cert, xw_append,
    xw_end_cert, xw_end_chain, xw_get_pkey
};

static long long unix_of(uint32_t days, uint32_t seconds)
{
    return ((long long)days - UNIX_EPOCH_DAYS) * 86400 + seconds;
}

/* BearSSL asks for each certificate: the dates are kept for the message
 * "check the PC's date and time", and t->now can be a test's fake time */
static int check_time(void *ctx, uint32_t nb_days, uint32_t nb_secs, uint32_t na_days, uint32_t na_secs)
{
    tls_conn *t = (tls_conn *)ctx;
    long long nb = unix_of(nb_days, nb_secs), na = unix_of(na_days, na_secs);
    int r = t->now < nb ? -1 : t->now > na ? 1 : 0;
    if (!t->dates_failed && (t->dates_seen == 0 || r != 0)) { t->not_before = nb; t->not_after = na; }
    t->dates_seen++;
    if (r) t->dates_failed = 1;
    return r;
}

/* days from 1970-01-01 to y-m-d (proleptic Gregorian) */
static long long days_from_civil(int y, int m, int d)
{
    long long era, yoe, doy;
    y -= m <= 2;
    era = (y >= 0 ? y : y - 399) / 400;
    yoe = y - era * 400;
    doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    return era * 146097 + yoe * 365 + yoe / 4 - yoe / 100 + doy - 719468;
}

long long tls_release_time(void)
{
    const char *v = RA_VERSION;
    int y = (v[0] - '0') * 10 + (v[1] - '0'), m = (v[2] - '0') * 10 + (v[3] - '0'),
        d = (v[4] - '0') * 10 + (v[5] - '0');
    return days_from_civil(2000 + y, m, d) * 86400;
}

static long long clock_now(void)
{
#ifdef _WIN32
    FILETIME ft; ULARGE_INTEGER u;
    GetSystemTimeAsFileTime(&ft);
    u.LowPart = ft.dwLowDateTime; u.HighPart = ft.dwHighDateTime;
    return (long long)(u.QuadPart / 10000000ULL) - 11644473600LL;
#else
    return (long long)time(NULL);
#endif
}

/* ---- the client ---- */

static const uint16_t suites[] = {
    BR_TLS_ECDHE_RSA_WITH_AES_128_GCM_SHA256,
    BR_TLS_ECDHE_ECDSA_WITH_AES_128_GCM_SHA256,
    BR_TLS_ECDHE_RSA_WITH_AES_256_GCM_SHA384,
    BR_TLS_ECDHE_ECDSA_WITH_AES_256_GCM_SHA384,
    BR_TLS_ECDHE_RSA_WITH_CHACHA20_POLY1305_SHA256,
    BR_TLS_ECDHE_ECDSA_WITH_CHACHA20_POLY1305_SHA256
};

/* SHA-1 only for the signatures of old chains; no MD5 */
static void set_hashes(tls_conn *t)
{
    static const struct { int id; const br_hash_class *hc; } h[] = {
        { br_sha1_ID, &br_sha1_vtable }, { br_sha224_ID, &br_sha224_vtable },
        { br_sha256_ID, &br_sha256_vtable }, { br_sha384_ID, &br_sha384_vtable },
        { br_sha512_ID, &br_sha512_vtable }
    };
    size_t i;
    for (i = 0; i < sizeof(h) / sizeof(h[0]); i++) {
        br_ssl_engine_set_hash(&t->cc.eng, h[i].id, h[i].hc);
        if (!t->x5.pin) br_x509_minimal_set_hash(&t->x5.minimal, h[i].id, h[i].hc);
    }
}

static void x509_setup(tls_conn *t)
{
    br_ssl_engine_context *eng = &t->cc.eng;
    memset(&t->x5, 0, sizeof(t->x5));
    t->x5.vtable = &xw_vtable;
    t->x5.pin = t->cfg.pin;
    if (t->x5.pin) return;
    br_x509_minimal_init(&t->x5.minimal, &br_sha256_vtable,
                         t->cfg.anchors->ta, t->cfg.anchors->n);
    br_x509_minimal_set_rsa(&t->x5.minimal, br_ssl_engine_get_rsavrfy(eng));
    br_x509_minimal_set_ecdsa(&t->x5.minimal, br_ssl_engine_get_ec(eng), br_ssl_engine_get_ecdsa(eng));
    br_x509_minimal_set_time_callback(&t->x5.minimal, t, check_time);
}

static void client_setup(tls_conn *t, const char *host)
{
    br_ssl_engine_context *eng = &t->cc.eng;
    br_ssl_client_zero(&t->cc);
    br_ssl_engine_set_versions(eng, BR_TLS12, BR_TLS12);
    br_ssl_engine_set_suites(eng, suites, sizeof(suites) / sizeof(suites[0]));
    br_ssl_engine_set_default_rsavrfy(eng);
    br_ssl_engine_set_default_ecdsa(eng);
    x509_setup(t);
    set_hashes(t);
    br_ssl_engine_set_prf_sha256(eng, &br_tls12_sha256_prf);
    br_ssl_engine_set_prf_sha384(eng, &br_tls12_sha384_prf);
    br_ssl_engine_set_default_aes_gcm(eng);
    br_ssl_engine_set_default_chapol(eng);
    br_ssl_engine_set_x509(eng, &t->x5.vtable);
    br_ssl_engine_set_buffer(eng, t->iobuf, sizeof(t->iobuf), 1);
    /* the name goes in SNI and is checked against the certificate */
    br_ssl_client_reset(&t->cc, host, 0);
}

/* the engine stopped: the network error that stopped it, or its own */
static int engine_closed(tls_conn *t)
{
    int err = br_ssl_engine_last_error(&t->cc.eng);
    t->last_err = err;
    return err == BR_ERR_OK ? NET_ERR_CLOSED : NET_ERR_IO;
}

/* the engine cannot be failed from outside (br_ssl_engine_fail is
 * internal): the connection remembers the network error and stays broken */
static int net_failed(tls_conn *t, int r)
{
    t->net_err = r;
    return r;
}

/* runs the engine until one of the target states: PUMP_READY, PUMP_NOTHING
 * (wait_ms >= 0 and no byte came within it) or a NET_ERR_* */
static int pump(tls_conn *t, unsigned target, int wait_ms)
{
    br_ssl_engine_context *eng = &t->cc.eng;
    for (;;) {
        unsigned st = br_ssl_engine_current_state(eng);
        unsigned char *buf; size_t len; int r;
        if (t->net_err) return t->net_err;
        if (st & BR_SSL_CLOSED) return engine_closed(t);
        if (st & BR_SSL_SENDREC) {
            buf = br_ssl_engine_sendrec_buf(eng, &len);
            r = net_send(t->net, buf, len);
            if (r != NET_OK) return net_failed(t, r);
            br_ssl_engine_sendrec_ack(eng, len);
            continue;
        }
        if (st & target) return PUMP_READY;
        /* writing while unread data blocks the engine: not in our use */
        if (st & BR_SSL_RECVAPP) return NET_ERR_IO;
        if (!(st & BR_SSL_RECVREC)) return NET_ERR_IO;
        buf = br_ssl_engine_recvrec_buf(eng, &len);
        r = wait_ms >= 0 ? net_recv_wait(t->net, buf, len, wait_ms) : net_recv(t->net, buf, len);
        if (r == 0) return PUMP_NOTHING;
        if (r < 0) return net_failed(t, r);
        br_ssl_engine_recvrec_ack(eng, (size_t)r);
    }
}

tls_conn *tls_new(const tls_config *cfg)
{
    tls_conn *t = (tls_conn *)calloc(1, sizeof(*t));
    if (!t) return NULL;
    t->cfg = *cfg;
    t->net = net_new();
    if (!t->net) { free(t); return NULL; }
    net_set_timeouts(t->net, cfg->connect_ms, cfg->io_ms);
    return t;
}

void tls_free(tls_conn *t)
{
    if (!t) return;
    tls_close(t);
    net_free(t->net);
    free(t);
}

net_conn *tls_net(tls_conn *t) { return t->net; }
int tls_is_open(const tls_conn *t) { return t->open && net_connected(t->net); }
int tls_last_error(const tls_conn *t) { return t->last_err; }

static int why_of(tls_conn *t, int r)
{
    if (r == NET_ERR_CANCELED) return TLS_ERR_CANCELED;
    if (t->net_err) return TLS_ERR_NET;
    if (t->x5.pin_bad) return TLS_ERR_PIN;
    switch (t->last_err) {
    case BR_ERR_X509_EXPIRED:         return TLS_ERR_CLOCK;
    case BR_ERR_X509_NOT_TRUSTED:     return TLS_ERR_UNKNOWN_ROOT;
    case BR_ERR_X509_BAD_SERVER_NAME: return TLS_ERR_BAD_NAME;
    case BR_ERR_IO:                   return TLS_ERR_NET;
    case BR_ERR_OK:                   return TLS_ERR_NET;   /* closed without a word */
    }
    return TLS_ERR_HANDSHAKE;
}

static void fill_info(tls_conn *t, tls_info *info)
{
    info->br_err = t->last_err;
    info->have_cert = t->x5.have_seen;
    if (t->x5.have_seen) memcpy(info->cert_sha256, t->x5.seen, 32);
    info->not_before = t->not_before;
    info->not_after = t->not_after;
    info->now = t->now;
}

static int open_fail(tls_conn *t, tls_info *info, int why, int net_err)
{
    info->why = why;
    info->net_err = net_err;
    fill_info(t, info);
    net_disconnect(t->net);
    return why;
}

int tls_open(tls_conn *t, const char *host, int port, tls_info *info)
{
    tls_info spare; int r;
    if (!info) info = &spare;
    memset(info, 0, sizeof(*info));
    tls_close(t);
    t->net_err = 0; t->last_err = 0;
    t->dates_seen = 0; t->dates_failed = 0; t->not_before = t->not_after = 0;
    t->now = t->cfg.now ? t->cfg.now : clock_now();
    memset(&t->x5, 0, sizeof(t->x5));
    if (!t->cfg.pin && !t->cfg.anchors) return open_fail(t, info, TLS_ERR_CONFIG, 0);
    /* a PC whose clock says a day before this program was released is
     * certainly wrong: say so without even trying the certificates */
    if (!t->cfg.pin && t->now < tls_release_time()) {
        t->not_before = tls_release_time();
        return open_fail(t, info, TLS_ERR_CLOCK, 0);
    }
    r = net_connect(t->net, host, port);
    if (r != NET_OK) return open_fail(t, info, r == NET_ERR_CANCELED ? TLS_ERR_CANCELED : TLS_ERR_NET, r);
    client_setup(t, host);
    r = pump(t, BR_SSL_SENDAPP, -1);
    if (r != PUMP_READY) {
        int why;
        t->last_err = br_ssl_engine_last_error(&t->cc.eng);
        why = why_of(t, r);
        return open_fail(t, info, why, why != TLS_ERR_NET ? 0 : r < 0 ? r : NET_ERR_IO);
    }
    t->open = 1;
    info->why = TLS_OK;
    fill_info(t, info);
    return TLS_OK;
}

int tls_write(tls_conn *t, const void *buf, size_t n)
{
    const unsigned char *p = (const unsigned char *)buf;
    if (!t->open) return NET_ERR_CLOSED;
    while (n > 0) {
        unsigned char *dst; size_t room;
        int r = pump(t, BR_SSL_SENDAPP, -1);
        if (r != PUMP_READY) return r < 0 ? r : NET_ERR_IO;
        dst = br_ssl_engine_sendapp_buf(&t->cc.eng, &room);
        if (room > n) room = n;
        memcpy(dst, p, room);
        br_ssl_engine_sendapp_ack(&t->cc.eng, room);
        p += room; n -= room;
    }
    return NET_OK;
}

int tls_flush(tls_conn *t)
{
    int r;
    if (!t->open) return NET_ERR_CLOSED;
    br_ssl_engine_flush(&t->cc.eng, 0);
    r = pump(t, BR_SSL_SENDAPP | BR_SSL_RECVAPP, -1);
    return r == PUMP_READY ? NET_OK : r < 0 ? r : NET_ERR_IO;
}

int tls_read_wait(tls_conn *t, void *buf, size_t n, int wait_ms)
{
    unsigned char *src; size_t avail; int r;
    if (!t->open) return NET_ERR_CLOSED;
    r = pump(t, BR_SSL_RECVAPP, wait_ms);
    if (r != PUMP_READY) return r;
    src = br_ssl_engine_recvapp_buf(&t->cc.eng, &avail);
    if (avail > n) avail = n;
    memcpy(buf, src, avail);
    br_ssl_engine_recvapp_ack(&t->cc.eng, avail);
    return (int)avail;
}

int tls_read(tls_conn *t, void *buf, size_t n)
{
    int r = tls_read_wait(t, buf, n, -1);
    return r == 0 ? NET_ERR_TIMEOUT : r;
}

void tls_close(tls_conn *t)
{
    br_ssl_engine_context *eng = &t->cc.eng;
    if (t->open && !t->net_err && net_connected(t->net) && !net_canceled(t->net)) {
        br_ssl_engine_close(eng);
        /* our close_notify goes out; the server's is not awaited */
        while (br_ssl_engine_current_state(eng) & BR_SSL_SENDREC) {
            size_t len; unsigned char *buf = br_ssl_engine_sendrec_buf(eng, &len);
            if (net_send(t->net, buf, len) != NET_OK) break;
            br_ssl_engine_sendrec_ack(eng, len);
        }
    }
    t->open = 0;
    net_disconnect(t->net);
}

const char *tls_strerror(int why)
{
    switch (why) {
    case TLS_OK:               return "ok";
    case TLS_ERR_NET:          return "network error";
    case TLS_ERR_CLOCK:        return "certificate expired or not yet valid (check the PC's date and time)";
    case TLS_ERR_UNKNOWN_ROOT: return "certificate signed by an unknown authority";
    case TLS_ERR_BAD_NAME:     return "certificate issued to another host";
    case TLS_ERR_PIN:          return "certificate different from the expected one";
    case TLS_ERR_HANDSHAKE:    return "secure connection refused";
    case TLS_ERR_CANCELED:     return "canceled";
    case TLS_ERR_CONFIG:       return "no certificates to trust";
    }
    return "unknown TLS error";
}

void tls_hex(const unsigned char d[32], char out[65])
{
    static const char hx[] = "0123456789abcdef";
    int i;
    for (i = 0; i < 32; i++) { out[2 * i] = hx[d[i] >> 4]; out[2 * i + 1] = hx[d[i] & 15]; }
    out[64] = 0;
}
