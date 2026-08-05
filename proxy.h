#ifndef PROXY_H
#define PROXY_H

// Attempt a short ENet connect to ip:port (IP or hostname) with timeout in ms.
// Returns 0 on successful ENet connect handshake, non-zero on failure.
int attempt_upstream_connect(const char* ip, int port, int timeout_ms);

#endif // PROXY_H
