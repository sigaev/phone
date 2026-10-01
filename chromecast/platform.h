#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "common/result.h"

// The phone's Wi-Fi networks. The app links the Android implementation;
// tests link a fake.
namespace chromecast {
struct Platform;

struct PhoneNetwork {
  // An Android net_handle_t for binding sockets.
  std::uint64_t handle = 0;
  // The phone's IPv4 address on the network, host byte order.
  std::uint32_t address = 0;
  int prefix = 24;
};

// A Wi-Fi network for Android to join on the app's behalf. Set ssid, an SSID
// prefix, or bssid. An empty passphrase selects an open network.
struct WifiRequest {
  std::string ssid, bssid, passphrase;
  bool prefix = false, wpa3 = false;
};

void destroy(Platform* platform) noexcept;
// Wi-Fi networks with an IPv4 address, including ones joined by request.
std::vector<PhoneNetwork> list_networks(Platform& platform);
// Android asks the person to approve the connection, then keeps it for this
// app until it is released. Returns a request identifier.
common::Result<int> request_network(Platform& platform, const WifiRequest& request);
void release_network(Platform& platform, int request);
}
