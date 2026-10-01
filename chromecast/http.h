#pragma once

#include <cstdint>
#include <expected>
#include <string>
#include <string_view>

namespace chromecast {
struct Endpoint {
  // An Android net_handle_t; 0 selects the process default network.
  std::uint64_t network = 0;
  // IPv4 in host byte order.
  std::uint32_t address = 0;
  std::uint16_t port = 0;
  // TLS without certificate verification: Chromecasts present self-signed
  // certificates for their setup API.
  bool tls = false;
};

struct Response {
  int status = 0;
  std::string body;
};

struct HttpError {
  std::string message;
  // The whole request was written before the connection failed. A Chromecast
  // that switches networks may drop the connection after accepting it.
  bool sent = false;
  // A TCP connection was made, so the failure came from TLS or HTTP.
  bool connected = false;
};

using HttpResult = std::expected<Response, HttpError>;

// One request on its own connection. Bodies are JSON; responses over 512 KiB
// are rejected. Each connect, read, and write waits at most timeout_ms.
HttpResult request(const Endpoint& endpoint, std::string_view method, std::string_view path,
                   std::string_view body = {}, int timeout_ms = 4000);
// Connect, then close, to test whether a TCP port accepts connections.
bool port_open(const Endpoint& endpoint, int timeout_ms);
// Open a non-blocking TCP connection that the caller polls for writability.
// Returns the socket, or -1.
int start_connect(const Endpoint& endpoint);
std::string format_address(std::uint32_t address);
}
