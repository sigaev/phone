#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "chromecast/platform.h"
#include "common/owner.h"
#include "common/result.h"

// A simulated Chromecast and phone for tests. The device serves its setup API
// over HTTPS and HTTP on loopback addresses: one stands for its home-network
// address and one for its setup hotspot. The fake Platform implements
// platform.h, adding a phone network whenever a request names one the device
// can be found on.
namespace chromecast {
struct FakeDevice;

struct FakeNetwork {
  std::string ssid, password;
  int auth = 7;
};

enum class FakeScanBehavior { kNormal, kEmpty, kReject, kDisconnect, kInvalid };

struct FakeDeviceConfig {
  std::uint32_t lan = 0x7f000001, hotspot = 0x7f000101;
  std::string name = "Chromecast1234", mac = "6C:AD:F8:00:12:34",
              hotspot_bssid = "FA:8F:CA:00:12:34";
  // Networks the device can see; the first is the one it starts on.
  std::vector<FakeNetwork> networks;
  // Start in setup mode, reachable only through the hotspot.
  bool setup_mode = false;
  bool keeps_hotspot = false;
  // How long the device is unreachable while it switches networks.
  double switch_seconds = .3;
  FakeScanBehavior scan = FakeScanBehavior::kNormal;
};

struct FakeRecord {
  int connects = 0, saves = 0;
  std::string last_ssid, last_password;
  bool keep_hotspot_requested = false;
  std::vector<std::string> configured;
};

// Ports are chosen by the system and shared by both addresses.
common::Result<common::Owner<FakeDevice>> create_fake_device(const FakeDeviceConfig& config);
void destroy(FakeDevice* device) noexcept;
std::uint16_t https_port(const FakeDevice& device);
std::uint16_t http_port(const FakeDevice& device);
FakeRecord get_record(FakeDevice& device);
// The SSID the device is on, empty in setup mode.
std::string current_ssid(FakeDevice& device);

enum class FakeRequestBehavior { kConnect, kWait, kError };

// The phone starts on the given home networks, each a loopback /24 address.
common::Owner<Platform> create_fake_platform(
    FakeDevice& device, const std::vector<std::string>& ssids,
    FakeRequestBehavior behavior = FakeRequestBehavior::kConnect);
std::vector<WifiRequest> get_requests(Platform& platform);
int active_requests(Platform& platform);
}
