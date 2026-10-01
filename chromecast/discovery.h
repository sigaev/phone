#pragma once

#include <cstdint>
#include <vector>

namespace chromecast {
inline constexpr std::uint32_t kMdnsAddress = 0xe00000fb;
inline constexpr std::uint16_t kMdnsPort = 5353;

// Addresses that answer a legacy-unicast mDNS query for _googlecast._tcp on
// the network whose local address is local. Replies come back unicast, so no
// multicast lock is needed.
std::vector<std::uint32_t> query_mdns(std::uint64_t network, std::uint32_t local,
                                      std::uint32_t group, std::uint16_t port, int timeout_ms);
// Hosts of the local subnet, at most the surrounding /24, that accept TCP
// connections on port. The local address itself is skipped.
std::vector<std::uint32_t> sweep_subnet(std::uint64_t network, std::uint32_t local, int prefix,
                                        std::uint16_t port, int timeout_ms);
}
