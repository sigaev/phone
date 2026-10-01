#include <poll.h>
#include <psa/crypto.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include <cstdio>
#include <string>

#include "chromecast/fakes.h"
#include "chromecast/session.h"

namespace {
using namespace chromecast;

int failures = 0;

void check(bool condition, const char* message) {
  if (!condition) {
    std::fprintf(stderr, "FAILED: %s\n", message);
    ++failures;
  }
}

double monotonic() {
  timespec now;
  clock_gettime(CLOCK_MONOTONIC, &now);
  return double(now.tv_sec) + now.tv_nsec * 1e-9;
}

const char* stage_name(Stage stage) {
  constexpr const char* kNames[] = {"searching", "devices", "hotspot", "scanning",
                                    "networks",  "sending", "joining", "finding",
                                    "saving",    "done",    "failed"};
  return kNames[int(stage)];
}

// Wait for a snapshot whose stage is wanted; fails after seconds.
Snapshot wait_for(Session& s, std::uint64_t command, Stage wanted, double seconds = 30) {
  double deadline = monotonic() + seconds;
  Snapshot last;
  while (monotonic() < deadline) {
    pollfd waiting{session_fd(s), POLLIN, 0};
    poll(&waiting, 1, 100);
    last = take_snapshot(s);
    if (last.command != command) continue;
    if (last.stage == wanted) return last;
    // Failures end every flow; report them instead of waiting.
    if (last.stage == Stage::kFailed && wanted != Stage::kFailed) break;
    if (last.stage == Stage::kDone && wanted != Stage::kDone) break;
  }
  std::fprintf(stderr, "waited for %s, stage is %s: %s\n", stage_name(wanted),
               stage_name(last.stage), last.message.c_str());
  return last;
}

struct World {
  common::Owner<FakeDevice> device;
  common::Owner<Platform> platform;
  common::Owner<Session> session;
};

World make_world(const FakeDeviceConfig& device, const std::vector<std::string>& phone,
                 const std::string& directory) {
  World w;
  auto created = create_fake_device(device);
  check(created.has_value(), "fake device starts");
  if (!created) return w;
  w.device = std::move(*created);
  w.platform = create_fake_platform(*w.device, phone);
  SessionConfig config;
  config.directory = directory;
  config.https_port = https_port(*w.device);
  config.http_port = http_port(*w.device);
  // Nothing answers mDNS; the subnet sweep finds the device.
  config.mdns_group = 0x7f000001;
  config.mdns_port = 9;
  config.hotspot_address = device.hotspot;
  config.time_scale = .05;
  auto session = create_session(*w.platform, config);
  check(session.has_value(), "session starts");
  if (session) w.session = std::move(*session);
  return w;
}

FakeDeviceConfig home_device() {
  FakeDeviceConfig c;
  c.networks = {{"Home", "home-password"}, {"Upstairs", "upstairs-pass"}, {"Cafe", "", 1}};
  return c;
}

int index_of(const Snapshot& s, const std::string& ssid) {
  for (std::size_t i = 0; i < s.networks.size(); ++i)
    if (s.networks[i].ssid == ssid) return int(i);
  return -1;
}

// The Chromecast is on the phone's network and moves to one the phone is not on.
void test_move_on_home_network(const std::string& directory) {
  World w = make_world(home_device(), {"Home"}, directory);
  if (!w.session) return;
  Session& s = *w.session;
  auto found = wait_for(s, search(s), Stage::kDevices);
  check(found.devices.size() == 1 && found.devices[0].reachable && !found.devices[0].hotspot,
        "the device is found on the home network");
  if (found.devices.empty()) return;
  check(found.devices[0].info.ssid == "Home" && found.devices[0].info.name == "Chromecast1234",
        "the device reports its network");
  auto listed = wait_for(s, open_device(s, 0), Stage::kNetworks);
  check(listed.networks.size() == 3 && index_of(listed, "Upstairs") >= 0,
        "the device's scan is listed");
  check(get_requests(*w.platform).empty(), "a reachable device needs no Wi-Fi request");
  auto done = wait_for(s, join(s, index_of(listed, "Upstairs"), "upstairs-pass"), Stage::kDone);
  FakeRecord record = get_record(*w.device);
  check(done.stage == Stage::kDone && current_ssid(*w.device) == "Upstairs",
        "the device moves to the new network");
  check(record.last_password == "upstairs-pass" && record.connects == 1 &&
            !record.keep_hotspot_requested,
        "the password reaches the device encrypted with its key");
  check(record.configured.size() == 2 && record.configured[1] == "Upstairs",
        "the new network is saved");
  auto requests = get_requests(*w.platform);
  check(requests.size() == 1 && requests[0].ssid == "Upstairs" &&
            requests[0].passphrase == "upstairs-pass" && !requests[0].prefix,
        "the phone joins the new network to find the device");
  check(active_requests(*w.platform) == 0, "Wi-Fi requests are released when done");
  check(done.message.find("Upstairs") != std::string::npos, "the result names the network");
}

// A wrong password sends the device back to its old network.
void test_wrong_password(const std::string& directory) {
  World w = make_world(home_device(), {"Home", "Upstairs"}, directory);
  if (!w.session) return;
  Session& s = *w.session;
  wait_for(s, search(s), Stage::kDevices);
  auto listed = wait_for(s, open_device(s, 0), Stage::kNetworks);
  auto failed =
      wait_for(s, join(s, index_of(listed, "Upstairs"), "not-the-password"), Stage::kFailed, 40);
  check(failed.stage == Stage::kFailed && failed.retry &&
            failed.message.find("Home") != std::string::npos,
        "a wrong password is reported with the old network");
  check(current_ssid(*w.device) == "Home" && get_record(*w.device).configured.size() == 1,
        "the device stays on its saved network");
  auto again = wait_for(s, rescan(s), Stage::kNetworks);
  check(again.networks.size() == 3, "trying again rescans");
  auto short_password = wait_for(s, join(s, index_of(again, "Upstairs"), "short"), Stage::kFailed);
  check(short_password.retry && get_record(*w.device).connects == 1,
        "invalid passphrases are never sent");
}

// The device cannot reach its network, so it is set up through its hotspot,
// which it keeps up until it has joined.
void test_hotspot_keeps(const std::string& directory) {
  FakeDeviceConfig c = home_device();
  c.setup_mode = true;
  c.keeps_hotspot = true;
  World w = make_world(c, {"Home"}, directory);
  if (!w.session) return;
  Session& s = *w.session;
  auto found = wait_for(s, search(s), Stage::kDevices);
  check(found.devices.empty(), "a device in setup mode is not on the home network");
  auto listed = wait_for(s, open_device(s, -1), Stage::kNetworks);
  check(listed.device.hotspot && listed.networks.size() == 3, "the hotspot is joined and scanned");
  auto requests = get_requests(*w.platform);
  check(requests.size() == 1 && requests[0].prefix && requests[0].ssid == "Chromecast",
        "unknown devices are found by their hotspot name");
  auto done = wait_for(s, join(s, index_of(listed, "Home"), "home-password"), Stage::kDone);
  FakeRecord record = get_record(*w.device);
  check(done.stage == Stage::kDone && record.keep_hotspot_requested &&
            record.configured.size() == 1 && record.configured[0] == "Home",
        "the device joins and saves through its hotspot");
  check(current_ssid(*w.device) == "Home" && active_requests(*w.platform) == 0,
        "the hotspot request is released");
}

// Without keeping its hotspot, the device is found again on the home network.
void test_hotspot_drops(const std::string& directory) {
  FakeDeviceConfig c = home_device();
  c.setup_mode = true;
  World w = make_world(c, {"Home"}, directory);
  if (!w.session) return;
  Session& s = *w.session;
  auto listed = wait_for(s, open_device(s, -1), Stage::kNetworks);
  check(listed.device.hotspot, "the hotspot is joined");
  auto done = wait_for(s, join(s, index_of(listed, "Home"), "home-password"), Stage::kDone);
  FakeRecord record = get_record(*w.device);
  check(done.stage == Stage::kDone && !record.keep_hotspot_requested && record.saves >= 1 &&
            record.configured.size() == 1,
        "the device is saved after it reappears on the home network");
  check(done.message.find("127.0.0.1") != std::string::npos, "the result shows its address");
}

// A device seen before but now out of reach is reached through its hotspot by BSSID.
void test_remembered(const std::string& directory) {
  FakeDeviceConfig c = home_device();
  c.setup_mode = true;
  c.keeps_hotspot = true;
  World w = make_world(c, {"Home"}, directory);
  if (!w.session) return;
  Session& s = *w.session;
  auto found = wait_for(s, search(s), Stage::kDevices);
  check(found.devices.size() == 1 && !found.devices[0].reachable &&
            found.devices[0].info.hotspot_bssid == c.hotspot_bssid,
        "a remembered device is listed as out of reach");
  auto listed = wait_for(s, open_device(s, 0), Stage::kNetworks);
  auto requests = get_requests(*w.platform);
  check(listed.device.hotspot && requests.size() == 1 && requests[0].bssid == c.hotspot_bssid,
        "its hotspot is requested by BSSID");
  // Wrong passwords are visible on a hotspot kept up while joining.
  auto failed = wait_for(s, join(s, index_of(listed, "Home"), "wrong-password"), Stage::kFailed);
  check(failed.retry && failed.message.find("Wrong password") != std::string::npos,
        "a wrong password is reported from the hotspot");
}
}

int main() {
  if (psa_crypto_init() != PSA_SUCCESS) return 1;
  char directory[] = "/data/data/com.termux/files/usr/tmp/chromecast-session.XXXXXX";
  char fallback[] = "/tmp/chromecast-session.XXXXXX";
  const char* root = mkdtemp(directory);
  if (!root) root = mkdtemp(fallback);
  if (!root) return 1;
  std::string first = std::string(root) + "/first", second = std::string(root) + "/second";
  mkdir(first.c_str(), 0700);
  mkdir(second.c_str(), 0700);
  test_move_on_home_network(first);
  test_wrong_password(second);
  // The hotspot tests share a directory; the first remembers the device.
  std::string third = std::string(root) + "/third";
  mkdir(third.c_str(), 0700);
  test_hotspot_keeps(third);
  test_remembered(third);
  test_hotspot_drops(second);
  std::string command = std::string("rm -rf ") + root;
  (void)!system(command.c_str());
  if (failures) return 1;
  std::printf("session_test passed\n");
  return 0;
}
