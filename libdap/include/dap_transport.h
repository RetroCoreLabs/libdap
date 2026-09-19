/**
 * @file dap_transport.h
 * @brief Transport layer definitions for the DAP library
 */

#ifndef ND100X_DAP_TRANSPORT_H
#define ND100X_DAP_TRANSPORT_H

#include <stddef.h>
#include <stdbool.h>
/* Pulls in the right socket headers for the platform (BSD sockets or Winsock2)
 * and defines dap_socket_t. Replaces the direct sys/socket.h + netinet + sys/un
 * + netdb includes that only existed on POSIX. */
#include "dap_net_compat.h"

/**
 * @brief Transport configuration
 */
typedef struct {
    enum {
        DAP_TRANSPORT_TCP,
        DAP_TRANSPORT_UNIX,
        DAP_TRANSPORT_PIPE,
        DAP_TRANSPORT_STDIO
    } type;
    union {
        struct {
            const char* host;
            int port;
        } tcp;
        struct {
            const char* path;
        } unix_socket;
        struct {
            const char* path;
        } pipe;
    } config;
} DAPTransportConfig;

/**
 * @brief Transport instance
 */
typedef struct DAPTransport {
    DAPTransportConfig config;
    /* dap_socket_t, not int: a Winsock SOCKET is an unsigned pointer-sized
     * handle, and storing it in an int would both truncate it on x64 and break
     * every "fd < 0" error test in the transport. */
    dap_socket_t listen_fd;
    dap_socket_t client_fd;
    bool is_server;    
    bool debuglog;
} DAPTransport;

/**
 * @brief Create a new transport instance
 * 
 * @param config Transport configuration
 * @return DAPTransport* New transport instance, or NULL on error
 */
DAPTransport* dap_transport_create(const DAPTransportConfig* config);

/**
 * @brief Start the transport
 * 
 * @param transport Transport instance
 * @return int 0 on success, -1 on error
 */
int dap_transport_start(DAPTransport* transport);

/**
 * @brief Stop the transport
 * 
 * @param transport Transport instance
 * @return int 0 on success, -1 on error
 */
int dap_transport_stop(DAPTransport* transport);

/**
 * @brief Free the transport
 * 
 * @param transport Transport instance
 */
void dap_transport_free(DAPTransport* transport);

/**
 * @brief Send data through the transport
 * 
 * @param transport Transport instance
 * @param data Data to send
 * @return int 0 on success, -1 on error
 */
int dap_transport_send(DAPTransport* transport, const char* data);

/**
 * @brief Receive data from the transport
 * 
 * @param transport Transport instance
 * @param message Output parameter for the received message (caller must free)
 * @return int 0 on success, -1 on error
 */
int dap_transport_receive(DAPTransport* transport, char** message);

/**
 * @brief Accept a new connection on the transport
 * 
 * @param transport Transport instance
 * @return int 0 on success, -1 on error
 */
int dap_transport_accept(DAPTransport* transport);

/**
 * @brief Connect to a server (client-side function)
 * 
 * @param transport Transport instance
 * @return int 0 on success, -1 on failure
 */
int dap_transport_connect(DAPTransport* transport);


/**
 * @brief Check if the transport is connected
 * 
 * @param transport Transport instance
 * @return int 0 on success, -1 on failure
 */
int dap_transport_is_connected(DAPTransport* transport);
#endif /* ND100X_DAP_TRANSPORT_H */ 

