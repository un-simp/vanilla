#ifndef VANILLA_OS_ABSTRACTION_H
#define VANILLA_OS_ABSTRACTION_H

#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>

/**
 * Use WPS PIN to authenticate with an access point
 */
int os_sync_with_console(const char *wireless_interface, uint16_t code);

/**
 * Connect to the access point using data acquired from `os_sync_with_console`.
 */
int os_connect_to_console(const char *wireless_interface, int (*callback)(void *), void *context);

/**
 * Open a UDP socket
 */
int os_open_udp_socket(void **socket_out, uint16_t port);
int os_close_udp_socket(void *socket);

/**
 * Read from socket opened by `os_open_udp_socket`.
 */
ssize_t os_read_from_udp_socket(const void *socket, void *data, size_t max);

/**
 * Write to socket opened by `os_open_udp_socket`.
 * 
 * Returns 1 on success or 0 on failure.
 */
int os_write_to_udp_socket(const void *from_socket, uint32_t to_address, uint16_t to_port, const void *data, size_t length);

#endif // VANILLA_OS_ABSTRACTION_H