/**
 * @file dap_net_compat.c
 * @brief Implementation of the libdap socket shim. See dap_net_compat.h.
 */

#include "dap_net_compat.h"

#include <string.h>
#include <stdio.h>

#ifdef _WIN32

/* Refcount so nested init/shutdown pairs (transport plus client, or libdap
 * plus the host program) cannot tear Winsock down while it is still in use. */
static int g_wsa_refcount = 0;

int dap_net_init(void) {
    WSADATA wsa;
    if (g_wsa_refcount > 0) { g_wsa_refcount++; return 0; }
    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) return -1;
    g_wsa_refcount = 1;
    return 0;
}

void dap_net_shutdown(void) {
    if (g_wsa_refcount <= 0) return;
    if (--g_wsa_refcount == 0) WSACleanup();
}

int dap_socket_close(dap_socket_t s) {
    if (s < 0) return 0;
    return closesocket(DAP_SOCK(s));
}

int dap_socket_errno(void) {
    return WSAGetLastError();
}

const char* dap_socket_strerror(int err) {
    /* FormatMessage would need a caller-supplied buffer and a LocalFree; a
     * static thread-unsafe buffer is enough for log lines and keeps the
     * signature identical to strerror(). */
    static char buf[256];
    DWORD n = FormatMessageA(FORMAT_MESSAGE_FROM_SYSTEM
                             | FORMAT_MESSAGE_IGNORE_INSERTS,
                             NULL, (DWORD)err,
                             MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT),
                             buf, (DWORD)sizeof buf, NULL);
    if (n == 0) {
        snprintf(buf, sizeof buf, "winsock error %d", err);
        return buf;
    }
    /* FormatMessage ends its strings with CRLF - drop it so log lines do not
     * gain a stray blank line. */
    while (n > 0 && (buf[n - 1] == '\r' || buf[n - 1] == '\n' || buf[n - 1] == ' '))
        buf[--n] = '\0';
    return buf;
}

int dap_socket_set_nonblocking(dap_socket_t s, int nonblock) {
    u_long mode = nonblock ? 1u : 0u;
    return (ioctlsocket(DAP_SOCK(s), FIONBIO, &mode) == 0) ? 0 : -1;
}

int dap_poll(dap_pollfd_t* fds, unsigned nfds, int timeout_ms) {
    /* WSAPoll needs Vista or later, which every supported Windows is. */
    return WSAPoll(fds, (ULONG)nfds, timeout_ms);
}

#else /* POSIX */

int dap_net_init(void) {
    return 0;                    /* nothing to start up */
}

void dap_net_shutdown(void) {
}

int dap_socket_close(dap_socket_t s) {
    if (s < 0) return 0;
    return close(s);
}

int dap_socket_errno(void) {
    return errno;
}

const char* dap_socket_strerror(int err) {
    return strerror(err);
}

int dap_socket_set_nonblocking(dap_socket_t s, int nonblock) {
    int flags = fcntl(s, F_GETFL, 0);
    if (flags < 0) return -1;
    flags = nonblock ? (flags | O_NONBLOCK) : (flags & ~O_NONBLOCK);
    return (fcntl(s, F_SETFL, flags) == 0) ? 0 : -1;
}

int dap_poll(dap_pollfd_t* fds, unsigned nfds, int timeout_ms) {
    return poll(fds, (nfds_t)nfds, timeout_ms);
}

#endif /* _WIN32 */
