/* https - a minimal HTTP/1.1 client over tls.c: one request at a time,
 * the connection kept open for the next one to the same host.
 *
 * Bodies go out with Content-Length (never chunked), from memory or from
 * a file read in pieces; responses come with Content-Length, chunked or
 * up to the close. A 307/308 redirect is followed only towards
 * https://archive.org or https://(anything).archive.org.
 *
 * Plain http exists only for the tests: http://127.0.0.1 in a build with
 * RA_TEST_BUILD or with RETROARCHIVER_IA_TEST=1, so that no setting can
 * make the program upload in clear text. */
#ifndef HTTPS_H
#define HTTPS_H
#include <stddef.h>
#include "strbuf.h"
#include "tls.h"

#define HTTPS_OK            0
#define HTTPS_ERR_URL      -1   /* malformed, or a scheme or host not allowed */
#define HTTPS_ERR_TLS      -2   /* no secure connection: tls_info says why */
#define HTTPS_ERR_NET      -3   /* the connection failed during the request: net_err */
#define HTTPS_ERR_PROTO    -4   /* the server's answer is not HTTP */
#define HTTPS_ERR_FILE     -5   /* the body's file cannot be read */
#define HTTPS_ERR_CANCELED -6   /* https_cancel, or the body callback said stop */
#define HTTPS_ERR_TOOBIG   -7   /* response body over HTTPS_MAX_BODY */

#define HTTPS_MAX_BODY      (16L * 1024 * 1024)
#define HTTPS_MAX_REDIRECTS 3
#define HTTPS_CONTINUE_MS   3000   /* wait for "100 Continue", then send anyway */
#define HTTPS_PIECE         16384  /* a file body is read and sent in pieces this big */

/* called with each piece of the body before it is sent; off is the piece's
 * position in the body (0 again if the body is sent again after a
 * redirect or a reconnection). Nonzero stops the request */
typedef int (*https_body_fn)(void *ctx, long long off, const void *data, size_t n);

typedef struct {
    const char *method;             /* "GET", "PUT", "POST", "DELETE", "HEAD" */
    const char *url;
    const char *headers;            /* more lines, each "Name: value\r\n"; NULL if none */
    const void *body;               /* the body from memory (body_len bytes), or */
    size_t body_len;
    const char *body_file;          /* from this file (UTF-8 path): */
    long long file_offset;          /* from this byte, */
    long long file_len;             /* this many bytes; 0 or less: up to the end */
    https_body_fn on_body;
    void *ctx;
    int expect_continue;            /* "Expect: 100-continue" with a body */
} https_request;

typedef struct {
    int status;                     /* the final status code (after redirects) */
    strbuf head;                    /* its status line and headers, as received */
    strbuf body;
    int redirects;                  /* how many were followed */
    int reused;                     /* the request went on an already open connection */
} https_response;

typedef struct https_client https_client;

https_client *https_new(const tls_config *cfg);
void https_free(https_client *c);
/* HTTPS_OK when a response arrived (whatever its status), or HTTPS_ERR_* */
int  https_do(https_client *c, const https_request *rq, https_response *rs);
/* details of the last failure */
const tls_info *https_tls_info(const https_client *c);
int  https_net_error(const https_client *c);
/* from another thread: the request in progress and the next ones end with
 * HTTPS_ERR_CANCELED, until https_uncancel */
void https_cancel(https_client *c);
void https_uncancel(https_client *c);
void https_disconnect(https_client *c);
const char *https_strerror(int code);

void https_response_init(https_response *rs);
void https_response_free(https_response *rs);
/* the value of a response header (first one, name in any case) into out; 0 if absent */
int  https_header(const https_response *rs, const char *name, char *out, size_t n);

/* percent-encoding (RFC 3986): letters, digits and -._~ stay, and so do
 * the characters in keep ("/" for a path); everything else, bytes of UTF-8
 * included, becomes %XX */
void https_pct(strbuf *out, const char *s, const char *keep);
/* appends name=value to a form (application/x-www-form-urlencoded), with
 * "&" before it if the form is not empty */
void https_form(strbuf *out, const char *name, const char *value);

/* parts of an absolute URL; 0 if malformed */
typedef struct { int tls; char host[256]; int port; char path[2048]; } https_url;
int  https_parse_url(const char *url, https_url *u);
/* may the client go to u? (https everywhere, plain http only in the test mode) */
int  https_url_allowed(const https_url *u);
/* may a redirect from "from" go to "to"? */
int  https_redirect_allowed(const https_url *from, const https_url *to);
/* the test mode: RA_TEST_BUILD or RETROARCHIVER_IA_TEST=1 */
int  https_test_mode(void);

#endif
