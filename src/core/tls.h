/* tls - a TLS 1.2 client (BearSSL) over net.c.
 *
 * Two ways to trust the server:
 *  - anchors: the roots of a CA bundle (cacert.pem), with the chain, its
 *    dates and the host name checked, as a browser does;
 *  - pin: the SHA-256 of the server certificate, confirmed once by the
 *    user (a NAS with a self-signed certificate): nothing else is checked.
 * Either way tls_info returns the SHA-256 of the certificate seen.
 *
 * Only ECDHE suites with AES-GCM or ChaCha20-Poly1305: no CBC, no static
 * RSA. A tls_conn can be opened again after tls_close, as a net_conn. */
#ifndef TLS_H
#define TLS_H
#include <stddef.h>
#include "net.h"

/* why a connection failed: the window picks its text from this */
#define TLS_OK              0
#define TLS_ERR_NET        -1   /* network: net_err says what */
#define TLS_ERR_CLOCK      -2   /* a certificate is expired or not yet valid: dates in tls_info;
                                 * or (not_after 0) the clock is before tls_release_time */
#define TLS_ERR_UNKNOWN_ROOT -3 /* the chain does not end at a known root */
#define TLS_ERR_BAD_NAME   -4   /* the certificate is for another host */
#define TLS_ERR_PIN        -5   /* the certificate is not the expected one */
#define TLS_ERR_HANDSHAKE  -6   /* refused by the server, or a bad certificate */
#define TLS_ERR_CANCELED   -7
#define TLS_ERR_CONFIG     -8   /* neither anchors nor pin */

typedef struct tls_anchors tls_anchors;

/* the certificates of a PEM file (CA bundle); NULL if it cannot be read
 * or holds no usable certificate. *count, if not NULL, gets how many */
tls_anchors *tls_anchors_load(const char *path, int *count);
void tls_anchors_free(tls_anchors *a);

typedef struct {
    const tls_anchors *anchors;      /* trust these roots, or */
    const unsigned char *pin;        /* only this certificate (32 bytes of SHA-256) */
    int connect_ms, io_ms;           /* 0: the net.c defaults */
    long long now;                   /* for the tests: the time to check the dates
                                      * against (Unix seconds); 0 = the PC clock */
} tls_config;

typedef struct {
    int why;                         /* TLS_OK or TLS_ERR_* */
    int net_err;                     /* NET_ERR_* with TLS_ERR_NET */
    int br_err;                      /* BearSSL's code, for the log */
    int have_cert;                   /* the server sent a certificate: */
    unsigned char cert_sha256[32];   /* its SHA-256 (DER) */
    /* validity of the certificate refused for its dates (TLS_ERR_CLOCK),
     * otherwise of the server certificate; Unix seconds, UTC */
    long long not_before, not_after;
    long long now;                   /* the time they were checked against */
} tls_info;

typedef struct tls_conn tls_conn;

tls_conn *tls_new(const tls_config *cfg);
void tls_free(tls_conn *t);
/* connects and completes the handshake: TLS_OK or the reason in info */
int  tls_open(tls_conn *t, const char *host, int port, tls_info *info);
int  tls_is_open(const tls_conn *t);
/* all n bytes, sent at tls_flush or when the record is full: NET_OK or NET_ERR_* */
int  tls_write(tls_conn *t, const void *buf, size_t n);
int  tls_flush(tls_conn *t);
/* up to n bytes: > 0, or NET_ERR_CLOSED at the end, or another NET_ERR_* */
int  tls_read(tls_conn *t, void *buf, size_t n);
/* like tls_read, waiting at most wait_ms for new bytes: 0 if none came */
int  tls_read_wait(tls_conn *t, void *buf, size_t n, int wait_ms);
/* sends close_notify without waiting for the answer, and disconnects */
void tls_close(tls_conn *t);
/* the TCP connection underneath, for net_cancel from another thread */
net_conn *tls_net(tls_conn *t);
/* BearSSL's error of the last failure, 0 if none */
int  tls_last_error(const tls_conn *t);
const char *tls_strerror(int why);
/* midnight UTC of the release day (RA_VERSION), Unix seconds */
long long tls_release_time(void);
/* the SHA-256 as lowercase hex (65 bytes with the terminator) */
void tls_hex(const unsigned char d[32], char out[65]);

#endif
