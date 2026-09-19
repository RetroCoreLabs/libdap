/**
 * @file dap_net_compat.h
 * @brief Cross-platform socket shim for libdap (POSIX / Winsock2).
 *
 * libdap was written straight against BSD sockets: int descriptors, errno for
 * errors, close(), fcntl(O_NONBLOCK) and AF_UNIX. None of that is spelled the
 * same way on Windows, so everything that differs is wrapped here rather than
 * scattered through dap_transport.c and dap_client.c.
 *
 * This header is deliberately SELF-CONTAINED - libdap is a submodule shared
 * with other emulators, so it must not reach into any particular parent
 * project's compat headers.
 *
 * The three things worth knowing:
 *
 *  1. Descriptors. Winsock's SOCKET is UNSIGNED, so the "fd < 0" tests all over
 *     this codebase would silently never fire. dap_socket_t is intptr_t on
 *     Windows, which keeps those comparisons meaningful, and INVALID_SOCKET
 *     ((SOCKET)~0) casts to -1 exactly as the code expects.
 *
 *  2. Errors. Winsock does NOT set errno. Every "if (errno == EINTR)" has to
 *     become dap_socket_errno(), and the constants differ too (WSAEINTR, not
 *     EINTR) - hence the DAP_E* names below.
 *
 *  3. AF_UNIX. Windows 10 build 17063 and later do support it, through
 *     <afunix.h>. Toolchains without that header still build; DAP_HAVE_AF_UNIX
 *     tells the transport whether to offer the UNIX-socket mode at all.
 */

#ifndef DAP_NET_COMPAT_H
#define DAP_NET_COMPAT_H

#include <stdint.h>
#include <stddef.h>

#ifdef _WIN32

#include <winsock2.h>
#include <ws2tcpip.h>
#include <mswsock.h>

/* AF_UNIX support arrived in Windows 10 1803 and lives in its own header.
 * Older MinGW headers do not ship it - build without UNIX sockets there rather
 * than failing to compile. */
#if defined(__has_include)
#  if __has_include(<afunix.h>)
#    include <afunix.h>
#    define DAP_HAVE_AF_UNIX 1
#  endif
#endif

typedef intptr_t   dap_socket_t;   /* signed, so the existing "< 0" tests work */
typedef int        dap_ssize_t;
typedef int        dap_socklen_t;
typedef WSAPOLLFD  dap_pollfd_t;   /* WSAPoll's struct; same layout as pollfd */

#define DAP_INVALID_SOCKET  ((dap_socket_t)-1)
/* Back to the native UINT_PTR SOCKET for the actual API calls. Casting through
 * int would truncate a handle on x64. */
#define DAP_SOCK(s)         ((SOCKET)(s))
#define DAP_SHUT_RDWR       SD_BOTH

/* send/recv take a char pointer and an int length on Winsock; POSIX takes a
 * void pointer and a size_t. These keep the casts in one place. */
#define DAP_BUF(p)          ((char*)(p))
#define DAP_CBUF(p)         ((const char*)(p))
#define DAP_LEN(n)          ((int)(n))
#define DAP_SOCKOPT(p)      ((const char*)(p))

/* Socket error codes. Winsock's live in a different namespace from errno. */
#define DAP_EINTR           WSAEINTR
#define DAP_EAGAIN          WSAEWOULDBLOCK
#define DAP_EWOULDBLOCK     WSAEWOULDBLOCK
#define DAP_ECONNRESET      WSAECONNRESET
#define DAP_EINPROGRESS     WSAEWOULDBLOCK   /* what a non-blocking connect gives */

#ifndef MSG_NOSIGNAL
#  define MSG_NOSIGNAL 0                     /* no SIGPIPE on Windows */
#endif

#else /* POSIX */

#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>
#include <sys/un.h>
#include <netdb.h>
#include <unistd.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>

#define DAP_HAVE_AF_UNIX 1

typedef int           dap_socket_t;
typedef ssize_t       dap_ssize_t;
typedef socklen_t     dap_socklen_t;
typedef struct pollfd dap_pollfd_t;

#define DAP_INVALID_SOCKET  ((dap_socket_t)-1)
#define DAP_SOCK(s)         (s)
#define DAP_SHUT_RDWR       SHUT_RDWR

#define DAP_BUF(p)          (p)
#define DAP_CBUF(p)         (p)
#define DAP_LEN(n)          ((size_t)(n))
#define DAP_SOCKOPT(p)      ((const void*)(p))

#define DAP_EINTR           EINTR
#define DAP_EAGAIN          EAGAIN
#define DAP_EWOULDBLOCK     EWOULDBLOCK
#define DAP_ECONNRESET      ECONNRESET
#define DAP_EINPROGRESS     EINPROGRESS

#ifndef MSG_NOSIGNAL
#  define MSG_NOSIGNAL 0                     /* macOS: SO_NOSIGPIPE instead */
#endif

#endif /* _WIN32 */

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Initialise the socket layer. WSAStartup on Windows, no-op on POSIX.
 *
 * Refcounted, so it is safe to call from more than one place. MUST have been
 * called before any other socket call on Windows, or every one of them fails
 * with WSANOTINITIALISED.
 *
 * @return 0 on success, -1 on failure.
 */
int  dap_net_init(void);

/** @brief Release one reference taken by dap_net_init(). */
void dap_net_shutdown(void);

/** @brief close() on POSIX, closesocket() on Windows. */
int  dap_socket_close(dap_socket_t s);

/**
 * @brief Last socket-layer error: errno on POSIX, WSAGetLastError() on Windows.
 *
 * Compare the result against the DAP_E* constants above, never against the
 * bare POSIX names - the numeric values are different on Windows.
 */
int  dap_socket_errno(void);

/** @brief Human-readable text for a code returned by dap_socket_errno(). */
const char* dap_socket_strerror(int err);

/**
 * @brief Put a socket into blocking or non-blocking mode.
 *
 * POSIX does this with fcntl(F_GETFL/F_SETFL, O_NONBLOCK); Winsock has no
 * fcntl at all and uses ioctlsocket(FIONBIO).
 *
 * @param s        socket
 * @param nonblock non-zero for non-blocking, zero for blocking
 * @return 0 on success, -1 on failure.
 */
int  dap_socket_set_nonblocking(dap_socket_t s, int nonblock);

/**
 * @brief poll() on POSIX, WSAPoll() on Windows.
 *
 * Winsock's WSAPoll only accepts SOCKETS - it cannot watch a file or a pipe the
 * way POSIX poll() can. That is fine here: libdap only ever polls its transport
 * socket.
 *
 * @return number of ready descriptors, 0 on timeout, -1 on error.
 */
int  dap_poll(dap_pollfd_t* fds, unsigned nfds, int timeout_ms);

#ifdef __cplusplus
}
#endif

#endif /* DAP_NET_COMPAT_H */
