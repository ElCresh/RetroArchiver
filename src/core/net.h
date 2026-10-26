/* net - a TCP connection with timeouts, under tls.c. Winsock on Windows,
 * BSD sockets elsewhere; names resolved with gethostbyname, which also
 * exists on Windows 2000 (getaddrinfo does not). IPv4 only.
 *
 * A net_conn is created once and can be connected again after
 * net_disconnect: whoever wants to stop it from another thread
 * (net_cancel) always has a valid object, until net_free. */
#ifndef NET_H
#define NET_H
#include <stddef.h>

#define NET_CONNECT_TIMEOUT_MS  30000
#define NET_IO_TIMEOUT_MS      120000   /* without a byte in or out */

#define NET_OK            0
#define NET_ERR_INIT     -1   /* Winsock does not start */
#define NET_ERR_RESOLVE  -2   /* the name does not resolve */
#define NET_ERR_CONNECT  -3   /* refused or unreachable */
#define NET_ERR_TIMEOUT  -4
#define NET_ERR_CLOSED   -5   /* the other side closed the connection */
#define NET_ERR_IO       -6   /* any other socket error */
#define NET_ERR_CANCELED -7   /* net_cancel */

typedef struct net_conn net_conn;

net_conn *net_new(void);
void net_free(net_conn *c);
/* 0 = the defaults above */
void net_set_timeouts(net_conn *c, int connect_ms, int io_ms);
/* NET_OK or an error; an open connection is closed first */
int  net_connect(net_conn *c, const char *host, int port);
int  net_connected(const net_conn *c);
/* all n bytes: NET_OK or an error */
int  net_send(net_conn *c, const void *buf, size_t n);
/* up to n bytes: how many arrived (> 0), or an error (NET_ERR_CLOSED at the end) */
int  net_recv(net_conn *c, void *buf, size_t n);
/* like net_recv, waiting at most wait_ms: 0 if nothing arrived by then */
int  net_recv_wait(net_conn *c, void *buf, size_t n, int wait_ms);
void net_disconnect(net_conn *c);
/* from another thread: the operation in progress and the next ones end
 * with NET_ERR_CANCELED, within a quarter of a second, until net_uncancel */
void net_cancel(net_conn *c);
void net_uncancel(net_conn *c);
int  net_canceled(const net_conn *c);
/* the system error code of the last failure (errno, WSAGetLastError), for the log */
int  net_syserr(const net_conn *c);
const char *net_strerror(int code);

#endif
