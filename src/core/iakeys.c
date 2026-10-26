/* Testing the IA-S3 keys: see iakeys.h */
#include "iakeys.h"
#include "https.h"
#include "json.h"
#include <stdio.h>
#include <string.h>

#define IA_TEST_CONNECT_MS 15000
#define IA_TEST_IO_MS      20000

/* the screen name, wherever the answer puts it: at the top or under the
 * "value" the tasks API wraps its answer in */
static void read_screenname(const char *body, char *out, size_t n)
{
    jval *root = json_parse(body, NULL, 0);
    const jval *value;
    const char *s;
    out[0] = 0;
    if (!root) return;
    if ((s = json_str(root, "screenname")) == NULL &&
        (value = json_get(root, "value")) != NULL)
        s = json_str(value, "screenname");
    if (s) str_copy(out, n, s);
    json_free(root);
}

/* an answer with "success": false: whatever the status, the keys are the
 * only thing this request can fail on */
static int says_failure(const char *body)
{
    jval *root = json_parse(body, NULL, 0);
    const jval *v;
    int no;
    if (!root) return 0;
    v = json_get(root, "success");
    no = v != NULL && v->t == J_BOOL && !v->b;
    json_free(root);
    return no;
}

static int from_status(const https_response *rs, ia_test *out)
{
    out->status = rs->status;
    if (rs->status == 401 || rs->status == 403) return IA_TEST_REFUSED;
    if (rs->status != 200) return IA_TEST_HTTP;
    if (says_failure(rs->body.s)) return IA_TEST_REFUSED;
    read_screenname(rs->body.s, out->screenname, sizeof(out->screenname));
    return IA_TEST_OK;
}

int ia_test_keys(const char *base, const char *cacert,
                 const char *access, const char *secret, ia_test *out)
{
    char url[512], auth[2 * 256 + 64];
    https_url u;
    tls_config cfg;
    tls_anchors *anchors = NULL;
    https_client *c;
    https_request rq;
    https_response rs;
    int r;

    memset(out, 0, sizeof(*out));
    snprintf(url, sizeof(url), "%s%s", base ? base : IA_BASE_URL, IA_TASKS_PATH);
    /* a base that is not a URL we may go to counts as unreachable: it can
     * only come from the test variable, never from the window */
    if (!https_parse_url(url, &u) || !https_url_allowed(&u)) {
        out->why = IA_TEST_NET;
        out->net_err = NET_ERR_RESOLVE;
        return out->why;
    }
    if (u.tls && (!cacert || !cacert[0] ||
                  (anchors = tls_anchors_load(cacert, NULL)) == NULL)) {
        out->why = IA_TEST_NO_CERTS;
        return out->why;
    }
    memset(&cfg, 0, sizeof(cfg));
    cfg.anchors = anchors;
    cfg.connect_ms = IA_TEST_CONNECT_MS;
    cfg.io_ms = IA_TEST_IO_MS;
    if ((c = https_new(&cfg)) == NULL) {
        tls_anchors_free(anchors);
        out->why = IA_TEST_NET;
        return out->why;
    }
    snprintf(auth, sizeof(auth), "Authorization: LOW %s:%s\r\nAccept: application/json\r\n",
             access ? access : "", secret ? secret : "");
    memset(&rq, 0, sizeof(rq));
    rq.method = "GET";
    rq.url = url;
    rq.headers = auth;
    https_response_init(&rs);
    r = https_do(c, &rq, &rs);
    memset(auth, 0, sizeof(auth));
    if (r == HTTPS_OK) {
        out->why = from_status(&rs, out);
    } else if (r == HTTPS_ERR_TLS) {
        out->why = IA_TEST_TLS;
        out->tls = *https_tls_info(c);
    } else if (r == HTTPS_ERR_PROTO) {
        out->why = IA_TEST_HTTP;
    } else {
        out->why = IA_TEST_NET;
        out->net_err = https_net_error(c);
    }
    https_response_free(&rs);
    https_free(c);
    tls_anchors_free(anchors);
    return out->why;
}
