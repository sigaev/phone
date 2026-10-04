#include "chromecast/http.h"

#include <android/multinetwork.h>
#include <arpa/inet.h>
#include <fcntl.h>
#include <mbedtls/ctr_drbg.h>
#include <mbedtls/entropy.h>
#include <mbedtls/net_sockets.h>
#include <mbedtls/ssl.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <new>

#include "common/owner.h"

namespace chromecast {
namespace {
constexpr std::size_t kMaximumResponse = 512 * 1024;

struct Connection {
  int fd = -1;
  bool tls = false;
  mbedtls_entropy_context entropy;
  mbedtls_ctr_drbg_context drbg;
  mbedtls_ssl_config config;
  mbedtls_ssl_context ssl;
};

void destroy(Connection* c) noexcept {
  if (c->tls) {
    mbedtls_ssl_free(&c->ssl);
    mbedtls_ssl_config_free(&c->config);
    mbedtls_ctr_drbg_free(&c->drbg);
    mbedtls_entropy_free(&c->entropy);
  }
  if (c->fd >= 0) close(c->fd);
  delete c;
}

std::unexpected<HttpError> failure(std::string message, bool sent = false, bool connected = false) {
  return std::unexpected(HttpError{std::move(message), sent, connected || sent});
}

sockaddr_in socket_address(const Endpoint& e) {
  sockaddr_in address{};
  address.sin_family = AF_INET;
  address.sin_port = htons(e.port);
  address.sin_addr.s_addr = htonl(e.address);
  return address;
}

int tls_send(void* context, const unsigned char* data, std::size_t size) {
  int fd = *static_cast<int*>(context);
  ssize_t n = send(fd, data, size, MSG_NOSIGNAL);
  if (n >= 0) return int(n);
  if (errno == EINTR) return MBEDTLS_ERR_SSL_WANT_WRITE;
  if (errno == EAGAIN || errno == EWOULDBLOCK) return MBEDTLS_ERR_SSL_TIMEOUT;
  return MBEDTLS_ERR_NET_SEND_FAILED;
}

int tls_receive(void* context, unsigned char* data, std::size_t size) {
  int fd = *static_cast<int*>(context);
  ssize_t n = recv(fd, data, size, 0);
  if (n >= 0) return int(n);
  if (errno == EINTR) return MBEDTLS_ERR_SSL_WANT_READ;
  if (errno == EAGAIN || errno == EWOULDBLOCK) return MBEDTLS_ERR_SSL_TIMEOUT;
  return MBEDTLS_ERR_NET_RECV_FAILED;
}

// Waits for a non-blocking connect, then makes the socket blocking with I/O timeouts.
bool finish_connect(int fd, int timeout_ms) {
  pollfd waiting{fd, POLLOUT, 0};
  int ready;
  do ready = poll(&waiting, 1, timeout_ms);
  while (ready < 0 && errno == EINTR);
  if (ready <= 0) return false;
  int error = 0;
  socklen_t size = sizeof(error);
  if (getsockopt(fd, SOL_SOCKET, SO_ERROR, &error, &size) != 0 || error) return false;
  int flags = fcntl(fd, F_GETFL);
  if (flags < 0 || fcntl(fd, F_SETFL, flags & ~O_NONBLOCK) != 0) return false;
  timeval limit{timeout_ms / 1000, timeout_ms % 1000 * 1000};
  return setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &limit, sizeof(limit)) == 0 &&
         setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &limit, sizeof(limit)) == 0;
}

std::expected<void, HttpError> start_tls(Connection& c) {
  mbedtls_entropy_init(&c.entropy);
  mbedtls_ctr_drbg_init(&c.drbg);
  mbedtls_ssl_config_init(&c.config);
  mbedtls_ssl_init(&c.ssl);
  c.tls = true;
  static constexpr unsigned char kPersonal[] = "chromecast-http";
  if (mbedtls_ctr_drbg_seed(&c.drbg, mbedtls_entropy_func, &c.entropy, kPersonal,
                            sizeof(kPersonal)) != 0)
    return failure("Cannot seed the random generator");
  if (mbedtls_ssl_config_defaults(&c.config, MBEDTLS_SSL_IS_CLIENT, MBEDTLS_SSL_TRANSPORT_STREAM,
                                  MBEDTLS_SSL_PRESET_DEFAULT) != 0)
    return failure("Cannot configure TLS");
  mbedtls_ssl_conf_authmode(&c.config, MBEDTLS_SSL_VERIFY_NONE);
  mbedtls_ssl_conf_rng(&c.config, mbedtls_ctr_drbg_random, &c.drbg);
  // Cast devices speak TLS 1.2.
  mbedtls_ssl_conf_max_tls_version(&c.config, MBEDTLS_SSL_VERSION_TLS1_2);
  if (mbedtls_ssl_setup(&c.ssl, &c.config) != 0) return failure("Cannot start TLS");
  mbedtls_ssl_set_bio(&c.ssl, &c.fd, tls_send, tls_receive, nullptr);
  int result;
  do result = mbedtls_ssl_handshake(&c.ssl);
  while (result == MBEDTLS_ERR_SSL_WANT_READ || result == MBEDTLS_ERR_SSL_WANT_WRITE);
  if (result == MBEDTLS_ERR_SSL_TIMEOUT) return failure("The TLS handshake timed out");
  if (result != 0) return failure("The TLS handshake failed");
  return {};
}

bool write_all(Connection& c, std::string_view data) {
  for (std::size_t offset = 0; offset < data.size();) {
    auto* bytes = reinterpret_cast<const unsigned char*>(data.data() + offset);
    int n = c.tls ? mbedtls_ssl_write(&c.ssl, bytes, data.size() - offset)
                  : int(send(c.fd, bytes, data.size() - offset, MSG_NOSIGNAL));
    if (n > 0) offset += n;
    else if (c.tls ? (n == MBEDTLS_ERR_SSL_WANT_READ || n == MBEDTLS_ERR_SSL_WANT_WRITE)
                   : (n < 0 && errno == EINTR))
      continue;
    else return false;
  }
  return true;
}

// Bytes read, 0 at end of stream, or -1 on failure.
int read_some(Connection& c, char* buffer, std::size_t size) {
  while (true) {
    if (!c.tls) {
      ssize_t n = recv(c.fd, buffer, size, 0);
      if (n < 0 && errno == EINTR) continue;
      return n < 0 ? -1 : int(n);
    }
    int n = mbedtls_ssl_read(&c.ssl, reinterpret_cast<unsigned char*>(buffer), size);
    if (n == MBEDTLS_ERR_SSL_WANT_READ || n == MBEDTLS_ERR_SSL_WANT_WRITE) continue;
    if (n == MBEDTLS_ERR_SSL_PEER_CLOSE_NOTIFY || n == MBEDTLS_ERR_NET_CONN_RESET) return 0;
    return n < 0 ? -1 : n;
  }
}

char lower(char c) { return c >= 'A' && c <= 'Z' ? char(c - 'A' + 'a') : c; }

bool same_text(std::string_view a, std::string_view b) {
  if (a.size() != b.size()) return false;
  for (std::size_t i = 0; i < a.size(); ++i)
    if (lower(a[i]) != lower(b[i])) return false;
  return true;
}

std::string_view trim(std::string_view value) {
  while (!value.empty() && (value.front() == ' ' || value.front() == '\t')) value.remove_prefix(1);
  while (!value.empty() && (value.back() == ' ' || value.back() == '\t' || value.back() == '\r'))
    value.remove_suffix(1);
  return value;
}

struct Head {
  int status = 0;
  std::size_t size = 0;
  long long length = -1;
  bool chunked = false;
};

bool parse_head(std::string_view raw, Head& head) {
  std::size_t end = raw.find("\r\n\r\n");
  if (end == std::string_view::npos) return false;
  head.size = end + 4;
  std::string_view lines = raw.substr(0, end);
  std::size_t line_end = lines.find("\r\n");
  std::string_view status = lines.substr(0, line_end);
  if (status.size() < 12 || status.substr(0, 5) != "HTTP/") return false;
  std::size_t space = status.find(' ');
  if (space == std::string_view::npos || space + 4 > status.size()) return false;
  head.status = 0;
  for (std::size_t i = space + 1; i < space + 4; ++i) {
    if (status[i] < '0' || status[i] > '9') return false;
    head.status = head.status * 10 + (status[i] - '0');
  }
  while (line_end != std::string_view::npos) {
    lines.remove_prefix(line_end + 2);
    line_end = lines.find("\r\n");
    std::string_view line = lines.substr(0, line_end);
    std::size_t colon = line.find(':');
    if (colon == std::string_view::npos) continue;
    std::string_view name = trim(line.substr(0, colon)), value = trim(line.substr(colon + 1));
    if (same_text(name, "content-length")) {
      head.length = 0;
      for (char c : value) {
        if (c < '0' || c > '9' || head.length > (1 << 30)) return false;
        head.length = head.length * 10 + (c - '0');
      }
    } else if (same_text(name, "transfer-encoding") && same_text(value, "chunked")) {
      head.chunked = true;
    }
  }
  return true;
}

// Returns true once the body is complete, filling body. Sets bad on malformed chunks.
bool decode_chunks(std::string_view data, std::string& body, bool& bad) {
  body.clear();
  while (true) {
    std::size_t line = data.find("\r\n");
    if (line == std::string_view::npos) return false;
    std::size_t size = 0;
    std::size_t digits = 0;
    for (char c : data.substr(0, line)) {
      int value = c >= '0' && c <= '9'   ? c - '0'
                  : c >= 'a' && c <= 'f' ? c - 'a' + 10
                  : c >= 'A' && c <= 'F' ? c - 'A' + 10
                                         : -1;
      if (value < 0) break;
      if (++digits > 7) {
        bad = true;
        return false;
      }
      size = size * 16 + value;
    }
    if (!digits) {
      bad = true;
      return false;
    }
    data.remove_prefix(line + 2);
    if (size == 0) return true;
    if (data.size() < size + 2) return false;
    body.append(data.substr(0, size));
    data.remove_prefix(size + 2);
  }
}

common::Owner<Connection> open_socket(const Endpoint& e) {
  common::Owner<Connection> c(new (std::nothrow) Connection);
  if (!c) return c;
  c->fd = start_connect(e);
  if (c->fd < 0) c.reset();
  return c;
}
}

std::string format_address(std::uint32_t address) {
  char text[16];
  std::snprintf(text, sizeof(text), "%u.%u.%u.%u", address >> 24, address >> 16 & 255,
                address >> 8 & 255, address & 255);
  return text;
}

int start_connect(const Endpoint& e) {
  int fd = socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, IPPROTO_TCP);
  if (fd < 0) return -1;
  if (e.network && android_setsocknetwork(e.network, fd) != 0) {
    close(fd);
    return -1;
  }
  sockaddr_in address = socket_address(e);
  if (connect(fd, reinterpret_cast<sockaddr*>(&address), sizeof(address)) != 0 &&
      errno != EINPROGRESS) {
    close(fd);
    return -1;
  }
  return fd;
}

bool port_open(const Endpoint& e, int timeout_ms) {
  auto c = open_socket(e);
  return c && finish_connect(c->fd, timeout_ms);
}

HttpResult request(const Endpoint& e, std::string_view method, std::string_view path,
                   std::string_view body, int timeout_ms) {
  std::string where = format_address(e.address) + ":" + std::to_string(e.port);
  auto c = open_socket(e);
  if (!c) return failure("Cannot open a connection to " + where);
  if (!finish_connect(c->fd, timeout_ms)) return failure("Cannot reach " + where);
  int one = 1;
  setsockopt(c->fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
  if (e.tls)
    if (auto started = start_tls(*c); !started)
      return failure(started.error().message, false, true);
  std::string message;
  message.reserve(256 + body.size());
  message.append(method).append(" ").append(path).append(" HTTP/1.1\r\nHost: ").append(where);
  // Chromecasts reject a charset parameter on connect_wifi.
  message.append(
      "\r\nContent-Type: application/json\r\nOrigin: https://www.google.com\r\n"
      "Accept: application/json\r\nConnection: close\r\n");
  if (method != "GET") message.append("Content-Length: ").append(std::to_string(body.size()));
  else message.resize(message.size() - 2);
  message.append("\r\n\r\n").append(body);
  if (!write_all(*c, message))
    return failure("The connection to " + where + " failed", false, true);
  std::string raw;
  Head head;
  bool have_head = false;
  char buffer[4096];
  while (true) {
    if (!have_head) have_head = parse_head(raw, head);
    if (have_head) {
      std::string_view rest = std::string_view(raw).substr(head.size);
      if (head.chunked) {
        std::string decoded;
        bool bad = false;
        if (decode_chunks(rest, decoded, bad)) return Response{head.status, std::move(decoded)};
        if (bad) return failure("Malformed response from " + where, true);
      } else if (head.length >= 0 && rest.size() >= std::size_t(head.length)) {
        return Response{head.status, std::string(rest.substr(0, head.length))};
      } else if (head.status == 204 || head.status == 304) {
        return Response{head.status, {}};
      }
    }
    int n = read_some(*c, buffer, sizeof(buffer));
    if (n < 0) return failure("No complete response from " + where, true);
    if (n == 0) {
      if (have_head && !head.chunked && head.length < 0)
        return Response{head.status, raw.substr(head.size)};
      return failure("The connection to " + where + " closed early", true);
    }
    raw.append(buffer, n);
    if (raw.size() > kMaximumResponse) return failure("The response is too large", true);
  }
}
}
