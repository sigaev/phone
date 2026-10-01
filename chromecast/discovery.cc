#include "chromecast/discovery.h"

#include <android/multinetwork.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <cstring>

#include "chromecast/http.h"

namespace chromecast {
namespace {
double monotonic_ms() {
  timespec now;
  clock_gettime(CLOCK_MONOTONIC, &now);
  return now.tv_sec * 1e3 + now.tv_nsec * 1e-6;
}

void add_unique(std::vector<std::uint32_t>& list, std::uint32_t address) {
  if (address && std::find(list.begin(), list.end(), address) == list.end())
    list.push_back(address);
}

// Skip a possibly compressed DNS name; returns the offset after it or 0.
std::size_t skip_name(const unsigned char* data, std::size_t size, std::size_t at) {
  for (int labels = 0; at < size && labels < 128; ++labels) {
    unsigned length = data[at];
    if (length == 0) return at + 1;
    if ((length & 0xc0) == 0xc0) return at + 2 <= size ? at + 2 : 0;
    if (length & 0xc0) return 0;
    at += length + 1;
  }
  return 0;
}

unsigned read16(const unsigned char* p) { return unsigned(p[0]) << 8 | p[1]; }

// IPv4 addresses from A records in any section of a response.
void collect_addresses(const unsigned char* data, std::size_t size,
                       std::vector<std::uint32_t>& out) {
  if (size < 12 || !(data[2] & 0x80)) return;
  unsigned questions = read16(data + 4);
  unsigned records = read16(data + 6) + read16(data + 8) + read16(data + 10);
  std::size_t at = 12;
  for (unsigned i = 0; i < questions; ++i) {
    at = skip_name(data, size, at);
    if (!at || at + 4 > size) return;
    at += 4;
  }
  for (unsigned i = 0; i < records; ++i) {
    at = skip_name(data, size, at);
    if (!at || at + 10 > size) return;
    unsigned type = read16(data + at), length = read16(data + at + 8);
    at += 10;
    if (at + length > size) return;
    if (type == 1 && length == 4)
      add_unique(out, std::uint32_t(data[at]) << 24 | std::uint32_t(data[at + 1]) << 16 |
                          std::uint32_t(data[at + 2]) << 8 | data[at + 3]);
    at += length;
  }
}
}

std::vector<std::uint32_t> query_mdns(std::uint64_t network, std::uint32_t local,
                                      std::uint32_t group, std::uint16_t port, int timeout_ms) {
  std::vector<std::uint32_t> found;
  int fd = socket(AF_INET, SOCK_DGRAM | SOCK_CLOEXEC, IPPROTO_UDP);
  if (fd < 0) return found;
  if (network && android_setsocknetwork(network, fd) != 0) {
    close(fd);
    return found;
  }
  sockaddr_in self{};
  self.sin_family = AF_INET;
  self.sin_addr.s_addr = htonl(local);
  in_addr interface{htonl(local)};
  unsigned char ttl = 255;
  bind(fd, reinterpret_cast<sockaddr*>(&self), sizeof(self));
  setsockopt(fd, IPPROTO_IP, IP_MULTICAST_IF, &interface, sizeof(interface));
  setsockopt(fd, IPPROTO_IP, IP_MULTICAST_TTL, &ttl, sizeof(ttl));
  // One PTR question for _googlecast._tcp.local with the unicast-response bit.
  static constexpr unsigned char kQuery[] = {0x12, 0x34, 0,   0,   0,   1,   0,   0,   0,    0,
                                             0,    0,    11,  '_', 'g', 'o', 'o', 'g', 'l',  'e',
                                             'c',  'a',  's', 't', 4,   '_', 't', 'c', 'p',  5,
                                             'l',  'o',  'c', 'a', 'l', 0,   0,   12,  0x80, 1};
  sockaddr_in target{};
  target.sin_family = AF_INET;
  target.sin_port = htons(port);
  target.sin_addr.s_addr = htonl(group);
  double start = monotonic_ms(), deadline = start + timeout_ms;
  // Repeat the query once, as mDNS queriers do, in case the first is lost.
  bool repeated = false;
  sendto(fd, kQuery, sizeof(kQuery), 0, reinterpret_cast<sockaddr*>(&target), sizeof(target));
  unsigned char packet[9000];
  while (true) {
    double now = monotonic_ms();
    if (now >= deadline) break;
    if (!repeated && now - start > timeout_ms / 3.) {
      repeated = true;
      sendto(fd, kQuery, sizeof(kQuery), 0, reinterpret_cast<sockaddr*>(&target), sizeof(target));
    }
    pollfd waiting{fd, POLLIN, 0};
    double next = repeated ? deadline : start + timeout_ms / 3.;
    int ready = poll(&waiting, 1, int(std::max(1., next - now)));
    if (ready < 0 && errno == EINTR) continue;
    if (ready < 0) break;
    if (ready == 0) continue;
    sockaddr_in source{};
    socklen_t length = sizeof(source);
    ssize_t n =
        recvfrom(fd, packet, sizeof(packet), 0, reinterpret_cast<sockaddr*>(&source), &length);
    if (n <= 0) continue;
    std::size_t before = found.size();
    collect_addresses(packet, std::size_t(n), found);
    std::uint32_t sender = ntohl(source.sin_addr.s_addr);
    // A responder that names no address is still the device itself.
    if (found.size() == before && n >= 12 && (packet[2] & 0x80) && read16(packet + 6))
      add_unique(found, sender);
  }
  close(fd);
  found.erase(std::remove(found.begin(), found.end(), local), found.end());
  return found;
}

std::vector<std::uint32_t> sweep_subnet(std::uint64_t network, std::uint32_t local, int prefix,
                                        std::uint16_t port, int timeout_ms) {
  std::vector<std::uint32_t> open;
  if (prefix < 24) prefix = 24;
  if (prefix > 30) return open;
  std::uint32_t mask = ~0u << (32 - prefix), base = local & mask;
  std::uint32_t count = ~mask + 1u;
  std::vector<pollfd> sockets;
  std::vector<std::uint32_t> hosts;
  for (std::uint32_t i = 1; i + 1 < count; ++i) {
    std::uint32_t host = base | i;
    if (host == local) continue;
    int fd = start_connect({network, host, port, false});
    if (fd < 0) continue;
    sockets.push_back({fd, POLLOUT, 0});
    hosts.push_back(host);
  }
  double deadline = monotonic_ms() + timeout_ms;
  std::size_t pending = sockets.size();
  while (pending) {
    double now = monotonic_ms();
    if (now >= deadline) break;
    int ready = poll(sockets.data(), sockets.size(), int(deadline - now) + 1);
    if (ready < 0 && errno == EINTR) continue;
    if (ready <= 0) break;
    for (std::size_t i = 0; i < sockets.size(); ++i) {
      if (sockets[i].fd < 0 || !sockets[i].revents) continue;
      int error = 0;
      socklen_t size = sizeof(error);
      if (getsockopt(sockets[i].fd, SOL_SOCKET, SO_ERROR, &error, &size) == 0 && !error &&
          (sockets[i].revents & POLLOUT))
        open.push_back(hosts[i]);
      close(sockets[i].fd);
      sockets[i].fd = -1;
      --pending;
    }
  }
  for (const pollfd& s : sockets)
    if (s.fd >= 0) close(s.fd);
  return open;
}
}
