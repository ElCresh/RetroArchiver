#include <stdlib.h>
#include <string.h>
#include "net.h"

#ifdef _WIN32
#include <winsock2.h>
#include <windows.h>
typedef SOCKET sock_t;
typedef int sock_len;
#define BAD_SOCK        INVALID_SOCKET
#define sock_close      closesocket
#define sock_errno()    WSAGetLastError()
#define SEND_FLAGS      0
#define SHUT_BOTH       SD_BOTH
#define IS_PENDING(e)   ((e) == WSAEWOULDBLOCK)
#define IS_AGAIN(e)     ((e) == WSAEWOULDBLOCK)
#define IS_INTR(e)      ((e) == WSAEINTR)
#define IS_REFUSED(e)   ((e) == WSAECONNREFUSED || (e) == WSAENETUNREACH || (e) == WSAEHOSTUNREACH)
#define IS_RESET(e)     ((e) == WSAECONNRESET || (e) == WSAECONNABORTED)
/* whoever takes the socket with the exchange closes it: net_disconnect
 * never closes a socket twice */
#define take_sock(p)    ((sock_t)InterlockedExchangePointer((PVOID volatile *)(p), (PVOID)BAD_SOCK))
#else
#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/select.h>
#include <sys/socket.h>
typedef int sock_t;
typedef socklen_t sock_len;
#define BAD_SOCK        (-1)
#define sock_close      close
#define sock_errno()    errno
#define SEND_FLAGS      MSG_NOSIGNAL    /* a closed peer is an error, not SIGPIPE */
#define SHUT_BOTH       SHUT_RDWR
#define IS_PENDING(e)   ((e) == EINPROGRESS)
#define IS_AGAIN(e)     ((e) == EAGAIN || (e) == EWOULDBLOCK)
#define IS_INTR(e)      ((e) == EINTR)
#define IS_REFUSED(e)   ((e) == ECONNREFUSED || (e) == ENETUNREACH || (e) == EHOSTUNREACH)
#define IS_RESET(e)     ((e) == ECONNRESET || (e) == EPIPE)
#define take_sock(p)    __sync_lock_test_and_set((p), BAD_SOCK)
#endif

/* waits are cut into slices, so that a cancellation is seen even where
 * shutdown does not wake select (a socket still connecting) */
#define SLICE_MS 250

struct net_conn {
    volatile sock_t s;
    volatile int canceled;
    int connect_ms, io_ms;
    int syserr;
};

net_conn *net_new(void)
{
    net_conn *c = (net_conn *)calloc(1, sizeof(*c));
#ifdef _WIN32
    WSADATA wd;
    /* reference counted: one per object, released by net_free */
    if (c && WSAStartup(MAKEWORD(2, 2), &wd) != 0) { free(c); return NULL; }
#endif
    if (!c) return NULL;
    c->s = BAD_SOCK;
    c->connect_ms = NET_CONNECT_TIMEOUT_MS;
    c->io_ms = NET_IO_TIMEOUT_MS;
    return c;
}

void net_free(net_conn *c)
{
    if (!c) return;
    net_disconnect(c);
#ifdef _WIN32
    WSACleanup();
#endif
    free(c);
}

void net_set_timeouts(net_conn *c, int connect_ms, int io_ms)
{
    c->connect_ms = connect_ms > 0 ? connect_ms : NET_CONNECT_TIMEOUT_MS;
    c->io_ms = io_ms > 0 ? io_ms : NET_IO_TIMEOUT_MS;
}

int net_connected(const net_conn *c) { return c->s != BAD_SOCK; }
int net_canceled(const net_conn *c) { return c->canceled; }
int net_syserr(const net_conn *c) { return c->syserr; }
void net_uncancel(net_conn *c) { c->canceled = 0; }

void net_cancel(net_conn *c)
{
    sock_t s = c->s;
    c->canceled = 1;
    /* wakes a recv or a select in progress; the owner closes the socket */
    if (s != BAD_SOCK) shutdown(s, SHUT_BOTH);
}

void net_disconnect(net_conn *c)
{
    sock_t s = take_sock(&c->s);
    if (s != BAD_SOCK) sock_close(s);
}

static int fail(net_conn *c, int code)
{
    c->syserr = sock_errno();
    return c->canceled ? NET_ERR_CANCELED : code;
}

/* until s can be written (write) or read, at most ms */
static int wait_sock(net_conn *c, sock_t s, int write, int ms)
{
    while (!c->canceled) {
        fd_set fs, es; struct timeval tv; int step = ms < SLICE_MS ? ms : SLICE_MS, r;
        if (step <= 0) return NET_ERR_TIMEOUT;
        FD_ZERO(&fs); FD_SET(s, &fs);
        FD_ZERO(&es); FD_SET(s, &es);       /* Windows reports a failed connect here */
        tv.tv_sec = step / 1000; tv.tv_usec = (step % 1000) * 1000;
        r = select((int)s + 1, write ? NULL : &fs, write ? &fs : NULL, &es, &tv);
        if (r > 0) return NET_OK;
        if (r < 0 && !IS_INTR(sock_errno())) return fail(c, NET_ERR_IO);
        ms -= step;
    }
    return NET_ERR_CANCELED;
}

static int set_nonblocking(sock_t s)
{
#ifdef _WIN32
    u_long on = 1;
    return ioctlsocket(s, FIONBIO, &on) == 0;
#else
    int fl = fcntl(s, F_GETFL, 0);
    return fl != -1 && fcntl(s, F_SETFL, fl | O_NONBLOCK) == 0;
#endif
}

static int resolve(const char *host, struct sockaddr_in *sa)
{
    struct hostent *he;
    memset(sa, 0, sizeof(*sa));
    sa->sin_family = AF_INET;
    sa->sin_addr.s_addr = inet_addr(host);
    if (sa->sin_addr.s_addr != INADDR_NONE) return 1;
    he = gethostbyname(host);
    if (!he || he->h_addrtype != AF_INET || !he->h_addr_list[0]) return 0;
    memcpy(&sa->sin_addr, he->h_addr_list[0], sizeof(sa->sin_addr));
    return 1;
}

/* the result of a non-blocking connect that select says is over */
static int connect_result(net_conn *c, sock_t s)
{
    int err = 0;
    sock_len len = sizeof(err);
    if (getsockopt(s, SOL_SOCKET, SO_ERROR, (char *)&err, &len) != 0) return fail(c, NET_ERR_CONNECT);
    if (err == 0) return NET_OK;
    c->syserr = err;
    return c->canceled ? NET_ERR_CANCELED : NET_ERR_CONNECT;
}

int net_connect(net_conn *c, const char *host, int port)
{
    struct sockaddr_in sa; sock_t s; int r, one = 1;
    net_disconnect(c);
    c->syserr = 0;
    if (c->canceled) return NET_ERR_CANCELED;
    if (!resolve(host, &sa)) return fail(c, NET_ERR_RESOLVE);
    sa.sin_port = htons((unsigned short)port);
    s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (s == BAD_SOCK) return fail(c, NET_ERR_IO);
    /* a request's head goes out at once, not after the delayed ACK */
    setsockopt(s, IPPROTO_TCP, TCP_NODELAY, (const char *)&one, sizeof(one));
    if (!set_nonblocking(s)) { r = fail(c, NET_ERR_IO); sock_close(s); return r; }
    c->s = s;
    if (connect(s, (struct sockaddr *)&sa, sizeof(sa)) != 0) {
        int e = sock_errno();
        if (!IS_PENDING(e)) { c->syserr = e; net_disconnect(c); return IS_REFUSED(e) ? NET_ERR_CONNECT : NET_ERR_IO; }
        r = wait_sock(c, s, 1, c->connect_ms);
        if (r == NET_OK) r = connect_result(c, s);
        if (r != NET_OK) { net_disconnect(c); return r; }
    }
    if (c->canceled) { net_disconnect(c); return NET_ERR_CANCELED; }
    return NET_OK;
}

int net_send(net_conn *c, const void *buf, size_t n)
{
    const char *p = (const char *)buf;
    sock_t s = c->s;
    if (s == BAD_SOCK) return c->canceled ? NET_ERR_CANCELED : NET_ERR_CLOSED;
    while (n > 0) {
        int chunk = n > 65536 ? 65536 : (int)n, r = wait_sock(c, s, 1, c->io_ms);
        if (r != NET_OK) return r;
        r = send(s, p, chunk, SEND_FLAGS);
        if (r < 0) {
            int e = sock_errno();
            if (IS_AGAIN(e) || IS_INTR(e)) continue;
            return fail(c, IS_RESET(e) ? NET_ERR_CLOSED : NET_ERR_IO);
        }
        p += r; n -= (size_t)r;
    }
    return c->canceled ? NET_ERR_CANCELED : NET_OK;
}

int net_recv_wait(net_conn *c, void *buf, size_t n, int wait_ms)
{
    sock_t s = c->s;
    int chunk = n > 65536 ? 65536 : (int)n;
    if (s == BAD_SOCK) return c->canceled ? NET_ERR_CANCELED : NET_ERR_CLOSED;
    for (;;) {
        int r = wait_sock(c, s, 0, wait_ms);
        if (r == NET_ERR_TIMEOUT) return 0;
        if (r != NET_OK) return r;
        r = recv(s, (char *)buf, chunk, 0);
        if (c->canceled) return NET_ERR_CANCELED;
        if (r > 0) return r;
        if (r == 0) return NET_ERR_CLOSED;
        {
            int e = sock_errno();
            if (IS_AGAIN(e) || IS_INTR(e)) continue;
            return fail(c, IS_RESET(e) ? NET_ERR_CLOSED : NET_ERR_IO);
        }
    }
}

int net_recv(net_conn *c, void *buf, size_t n)
{
    int r = net_recv_wait(c, buf, n, c->io_ms);
    return r == 0 ? NET_ERR_TIMEOUT : r;
}

const char *net_strerror(int code)
{
    switch (code) {
    case NET_OK:           return "ok";
    case NET_ERR_INIT:     return "network not available";
    case NET_ERR_RESOLVE:  return "host name not found";
    case NET_ERR_CONNECT:  return "connection refused or host unreachable";
    case NET_ERR_TIMEOUT:  return "timed out";
    case NET_ERR_CLOSED:   return "connection closed by the server";
    case NET_ERR_IO:       return "network error";
    case NET_ERR_CANCELED: return "canceled";
    }
    return "unknown network error";
}
