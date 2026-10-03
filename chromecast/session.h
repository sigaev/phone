#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "chromecast/discovery.h"
#include "chromecast/http.h"
#include "chromecast/protocol.h"
#include "common/owner.h"
#include "common/result.h"

namespace chromecast {
struct Platform;
struct Session;

struct Device {
  DeviceInfo info;
  // Where its setup API answered last; address 0 when it is out of reach.
  Endpoint endpoint;
  bool reachable = false;
  // Reached through its setup hotspot rather than a home network.
  bool hotspot = false;
};

enum class Stage {
  kSearching,
  kDevices,
  // Android is joining a Chromecast's setup hotspot.
  kHotspot,
  // The Chromecast is scanning for Wi-Fi networks.
  kScanning,
  kNetworks,
  kSending,
  kJoining,
  // Looking for the Chromecast on the phone's Wi-Fi networks after it moved.
  kFinding,
  kSaving,
  kDone,
  kFailed
};

struct Snapshot {
  Stage stage = Stage::kSearching;
  std::vector<Device> devices;
  Device device;
  std::vector<WifiNetwork> networks;
  // The network being joined and a result or failure for people.
  std::string target, message;
  // Android is showing, or may show, a prompt to join a Wi-Fi network.
  bool prompt = false;
  // Confirmed by this device's setup API, never inferred from a disconnect.
  bool joined = false;
  // A failed join can be retried with another password.
  bool retry = false;
  // The stage that failed when stage is kFailed.
  Stage failed = Stage::kFailed;
  // The command this snapshot reports on, as numbered by the calls below.
  std::uint64_t command = 0;
};

struct SessionConfig {
  // Where the list of Chromecasts seen before is kept.
  std::string directory;
  std::uint16_t http_port = 8008, https_port = 8443;
  std::uint32_t mdns_group = kMdnsAddress;
  std::uint16_t mdns_port = kMdnsPort;
  // A Chromecast's setup hotspot gives it this address, 192.168.255.249.
  std::uint32_t hotspot_address = 0xc0a8fff9;
  // Scales every wait and deadline, so tests can run the flows quickly.
  double time_scale = 1;
};

// Work happens on a background thread. Each command replaces the running one
// and returns its number.
common::Result<common::Owner<Session>> create_session(Platform& platform,
                                                      const SessionConfig& config);
void destroy(Session* session) noexcept;
// Readable when the snapshot has changed.
int session_fd(const Session& session);
Snapshot take_snapshot(Session& session);
// Find Chromecasts on every Wi-Fi network the phone has, and list them with
// the ones seen before. Releases any Wi-Fi networks the app requested.
std::uint64_t search(Session& session);
// List the Wi-Fi networks a device sees. Out of reach, or with index -1 for a
// Chromecast never seen, the phone joins its setup hotspot first.
std::uint64_t open_device(Session& session, int index);
// Scan again from the open device, rejoining its hotspot if it was lost.
std::uint64_t rescan(Session& session);
// Move the open device to the snapshot's network at index.
std::uint64_t join(Session& session, int network, std::string password);
}
