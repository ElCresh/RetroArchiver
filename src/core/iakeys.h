/* iakeys - does Archive.org accept these IA-S3 keys?
 *
 * One authenticated request to the tasks API, which takes the IA-S3 keys
 * and nothing else, so its answer says whether the keys are good without
 * creating or changing anything. Read only: nothing is uploaded here.
 *
 * The keys are passed in, used for the Authorization header and never
 * written anywhere: not in the result, not in a message. */
#ifndef IAKEYS_H
#define IAKEYS_H
#include "tls.h"

/* why in ia_test: what the window has to say */
#define IA_TEST_OK        0   /* the keys work */
#define IA_TEST_REFUSED   1   /* Archive.org does not accept them */
#define IA_TEST_NO_CERTS  2   /* the CA bundle is missing or unusable */
#define IA_TEST_TLS       3   /* no secure connection: tls says why (clock, root, name) */
#define IA_TEST_NET       4   /* the network: net_err says what */
#define IA_TEST_HTTP      5   /* an answer we do not understand: status */

#define IA_BASE_URL   "https://archive.org"
#define IA_TASKS_PATH "/services/tasks.php?summary=1"

typedef struct {
    int why;
    int status;                  /* the HTTP status, when one arrived */
    int net_err;                 /* NET_ERR_* with IA_TEST_NET */
    char screenname[128];        /* the account's name, if the answer gives one */
    tls_info tls;                /* with IA_TEST_TLS: the reason and the dates */
} ia_test;

/* base NULL is IA_BASE_URL; cacert is the CA bundle (cacert.pem), needed
 * only for an https URL. Returns out->why */
int ia_test_keys(const char *base, const char *cacert,
                 const char *access, const char *secret, ia_test *out);

#endif
