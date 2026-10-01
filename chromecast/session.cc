#include "chromecast/session.h"

#include <arpa/inet.h>
#include <pthread.h>
#include <sys/eventfd.h>
#include <time.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <new>

#include "chromecast/json.h"
#include "chromecast/platform.h"
#include "chromecast/storage.h"

namespace chromecast {
namespace {
enum class Command { kNone, kSearch, kOpen, kRescan, kJoin, kStop };
constexpr char kDevicesFile[] = "devices.json";
}

struct Session {
  Platform* platform = nullptr;
  SessionConfig config;
  pthread_t thread{};
  bool started = false;
  pthread_mutex_t mutex = PTHREAD_MUTEX_INITIALIZER;
  pthread_cond_t wake;
  bool wake_ready = false;
  int fd = -1;
  // Guarded by mutex.
  Command command = Command::kNone;
  int argument = -1;
  std::string password;
  std::uint64_t sequence = 0, pending = 0;
  Snapshot shared;
  std::atomic<bool> interrupted{false};
  // Used only by the worker thread.
  Snapshot work;
  std::uint64_t current = 0;
  std::vector<DeviceInfo> known;
  int hotspot_request = -1, target_request = -1;
};

namespace {
double monotonic() {
  timespec now;
  clock_gettime(CLOCK_MONOTONIC, &now);
  return double(now.tv_sec) + now.tv_nsec * 1e-9;
}

void publish(Session& s) {
  pthread_mutex_lock(&s.mutex);
  s.shared = s.work;
  s.shared.command = s.current;
  pthread_mutex_unlock(&s.mutex);
  const std::uint64_t one = 1;
  while (write(s.fd, &one, sizeof(one)) < 0 && errno == EINTR) {}
}

bool interrupted(const Session& s) { return s.interrupted.load(std::memory_order_relaxed); }

double scaled(const Session& s, double seconds) { return seconds * s.config.time_scale; }

int scaled_ms(const Session& s, double seconds) {
  return std::max(50, int(scaled(s, seconds) * 1000));
}

// Wait for a scaled number of seconds; false when a new command arrived.
bool pause(Session& s, double seconds) {
  timespec until;
  clock_gettime(CLOCK_MONOTONIC, &until);
  double wait = scaled(s, seconds);
  long nanoseconds = until.tv_nsec + long((wait - long(wait)) * 1e9);
  until.tv_sec += long(wait) + nanoseconds / 1000000000;
  until.tv_nsec = nanoseconds % 1000000000;
  pthread_mutex_lock(&s.mutex);
  while (s.command == Command::kNone)
    if (pthread_cond_timedwait(&s.wake, &s.mutex, &until) == ETIMEDOUT) break;
  bool idle = s.command == Command::kNone;
  pthread_mutex_unlock(&s.mutex);
  return idle;
}

double since(const Session& s, double start) { return (monotonic() - start) / s.config.time_scale; }

bool in_subnet(std::uint32_t a, std::uint32_t b, int prefix) {
  std::uint32_t mask = prefix <= 0 ? 0 : ~0u << (32 - std::min(prefix, 32));
  return (a & mask) == (b & mask);
}

bool on_hotspot(const Session& s, const PhoneNetwork& n) {
  return in_subnet(n.address, s.config.hotspot_address, 24);
}

std::uint32_t parse_address(const std::string& text) {
  in_addr parsed;
  return inet_pton(AF_INET, text.c_str(), &parsed) == 1 ? ntohl(parsed.s_addr) : 0;
}

bool identified(const DeviceInfo& d) {
  return !d.mac.empty() || !d.udn.empty() || !d.public_key.empty();
}

std::string name_of(const Device& d) {
  return d.info.name.empty() ? "The Chromecast" : d.info.name;
}

void release_requests(Session& s) {
  if (s.hotspot_request >= 0) release_network(*s.platform, s.hotspot_request);
  if (s.target_request >= 0) release_network(*s.platform, s.target_request);
  s.hotspot_request = s.target_request = -1;
  s.work.prompt = false;
}

// Remembered devices.

std::string encode_known(const std::vector<DeviceInfo>& known) {
  std::string out = "[";
  for (const DeviceInfo& d : known) {
    if (out.size() > 1) out += ",";
    out += "{\"name\":" + quote(d.name) + ",\"model\":" + quote(d.model) +
           ",\"mac\":" + quote(d.mac) + ",\"udn\":" + quote(d.udn) +
           ",\"hotspot_bssid\":" + quote(d.hotspot_bssid) + ",\"ssid\":" + quote(d.ssid) +
           ",\"ip\":" + quote(d.ip) + "}";
  }
  return out + "]";
}

std::vector<DeviceInfo> decode_known(std::string_view text) {
  std::vector<DeviceInfo> known;
  auto root = parse_json(text);
  if (!root || root->type != Json::Type::kArray) return known;
  for (const Json& row : root->items) {
    DeviceInfo d;
    d.name = as_string(find(&row, {"name"})).value_or("");
    d.model = as_string(find(&row, {"model"})).value_or("");
    d.mac = as_string(find(&row, {"mac"})).value_or("");
    d.udn = as_string(find(&row, {"udn"})).value_or("");
    d.hotspot_bssid = as_string(find(&row, {"hotspot_bssid"})).value_or("");
    d.ssid = as_string(find(&row, {"ssid"})).value_or("");
    d.ip = as_string(find(&row, {"ip"})).value_or("");
    if (!d.mac.empty() || !d.udn.empty()) known.push_back(std::move(d));
  }
  return known;
}

void save_known(Session& s) {
  std::string text = encode_known(s.known);
  auto bytes = std::as_bytes(std::span(text.data(), text.size()));
  (void)save_file(s.config.directory.c_str(), kDevicesFile, bytes);
}

void remember(Session& s, const DeviceInfo& info) {
  if (info.mac.empty() && info.udn.empty()) return;
  DeviceInfo kept = info;
  kept.public_key.clear();
  auto same = std::find_if(s.known.begin(), s.known.end(),
                           [&](const DeviceInfo& d) { return same_device(d, info); });
  if (same == s.known.end()) {
    s.known.push_back(kept);
    return;
  }
  // A hotspot reports no home network; keep the last one seen.
  if (kept.ssid.empty() || kept.ip.empty() ||
      in_subnet(parse_address(kept.ip), s.config.hotspot_address, 24)) {
    kept.ssid = same->ssid;
    kept.ip = same->ip;
  }
  *same = kept;
}

// Device API access.

Endpoint setup_endpoint(const Session& s, std::uint64_t network, std::uint32_t address) {
  return {network, address, s.config.https_port, true};
}

// Read device info over HTTPS, falling back to plain HTTP when TLS fails. The
// endpoint keeps the transport that answered.
std::optional<DeviceInfo> read_info(const Session& s, Endpoint& e, int timeout_ms) {
  for (int attempt = 0; attempt < 2; ++attempt) {
    auto response = request(e, "GET", kInfoPath, {}, timeout_ms);
    if (response && (response->status == 400 || response->status == 404))
      response = request(e, "GET", kBasicInfoPath, {}, timeout_ms);
    if (response) {
      if (response->status != 200) return std::nullopt;
      return parse_info(response->body);
    }
    if (!response.error().connected || !e.tls) return std::nullopt;
    e.tls = false;
    e.port = s.config.http_port;
  }
  return std::nullopt;
}

HttpResult post(const Endpoint& e, const char* path, std::string_view body = {}) {
  return request(e, "POST", path, body, 6000);
}

void add_device(std::vector<Device>& list, Device device) {
  auto same = std::find_if(list.begin(), list.end(),
                           [&](const Device& d) { return same_device(d.info, device.info); });
  if (same == list.end()) list.push_back(std::move(device));
  else if (same->hotspot && !device.hotspot) *same = std::move(device);
}

// Cast devices answering the setup API on one of the phone's networks.
std::vector<Device> find_devices(Session& s, const PhoneNetwork& n,
                                 const std::vector<std::uint32_t>& hints) {
  std::vector<Device> found;
  std::vector<std::uint32_t> candidates;
  bool hotspot = on_hotspot(s, n);
  if (hotspot) {
    candidates.push_back(s.config.hotspot_address);
  } else {
    for (std::uint32_t hint : hints)
      if (hint && in_subnet(hint, n.address, std::max(n.prefix, 24))) candidates.push_back(hint);
    for (std::uint32_t a : query_mdns(n.handle, n.address, s.config.mdns_group, s.config.mdns_port,
                                      scaled_ms(s, 1.2)))
      if (std::find(candidates.begin(), candidates.end(), a) == candidates.end())
        candidates.push_back(a);
    if (interrupted(s)) return found;
    for (std::uint32_t a :
         sweep_subnet(n.handle, n.address, n.prefix, s.config.http_port, scaled_ms(s, .8)))
      if (std::find(candidates.begin(), candidates.end(), a) == candidates.end())
        candidates.push_back(a);
  }
  for (std::uint32_t address : candidates) {
    if (interrupted(s)) break;
    Endpoint e = setup_endpoint(s, n.handle, address);
    if (auto info = read_info(s, e, 2500)) add_device(found, {*info, e, true, hotspot});
  }
  return found;
}

void fail(Session& s, std::string message, bool retry) {
  release_requests(s);
  s.work.failed = s.work.stage;
  s.work.stage = Stage::kFailed;
  s.work.message = std::move(message);
  s.work.retry = retry;
  publish(s);
}

// Commands.

void run_search(Session& s) {
  release_requests(s);
  s.work = {};
  s.work.stage = Stage::kSearching;
  for (const DeviceInfo& d : s.known) s.work.devices.push_back({d, {}, false, false});
  publish(s);
  std::vector<std::uint32_t> hints;
  for (const DeviceInfo& d : s.known) hints.push_back(parse_address(d.ip));
  std::vector<Device> found;
  for (const PhoneNetwork& n : list_networks(*s.platform)) {
    if (interrupted(s)) return;
    for (Device& d : find_devices(s, n, hints)) add_device(found, std::move(d));
  }
  if (interrupted(s)) return;
  for (const Device& d : found) remember(s, d.info);
  save_known(s);
  std::stable_partition(found.begin(), found.end(), [](const Device& d) { return !d.hotspot; });
  for (const DeviceInfo& d : s.known)
    if (std::none_of(found.begin(), found.end(),
                     [&](const Device& f) { return same_device(f.info, d); }))
      found.push_back({d, {}, false, false});
  s.work.devices = std::move(found);
  s.work.stage = Stage::kDevices;
  publish(s);
}

// Ask Android to join the device's setup hotspot and wait for its API.
bool join_hotspot(Session& s, Device& d) {
  s.work.stage = Stage::kHotspot;
  s.work.prompt = true;
  publish(s);
  WifiRequest r;
  // The hotspot BSSID is fixed per device; SSIDs start with the device name.
  if (!d.info.hotspot_bssid.empty()) r.bssid = d.info.hotspot_bssid;
  else {
    r.ssid = d.info.name.empty() ? "Chromecast" : d.info.name;
    r.prefix = true;
  }
  auto requested = request_network(*s.platform, r);
  if (!requested) {
    fail(s, "Android did not start joining the hotspot: " + requested.error().message, false);
    return false;
  }
  s.hotspot_request = *requested;
  double start = monotonic();
  bool widened = r.bssid.empty();
  while (since(s, start) < 150) {
    for (const PhoneNetwork& n : list_networks(*s.platform)) {
      if (interrupted(s)) return false;
      if (!on_hotspot(s, n)) continue;
      Endpoint e = setup_endpoint(s, n.handle, s.config.hotspot_address);
      auto info = read_info(s, e, 1500);
      if (!info || (identified(d.info) && !same_device(*info, d.info))) continue;
      d.info = *info;
      d.endpoint = e;
      d.reachable = d.hotspot = true;
      s.work.prompt = false;
      return true;
    }
    // Fall back to the SSID in case the hotspot uses another BSSID.
    if (!widened && since(s, start) > 45) {
      widened = true;
      release_network(*s.platform, s.hotspot_request);
      s.hotspot_request = -1;
      WifiRequest named;
      named.ssid = d.info.name.empty() ? "Chromecast" : d.info.name;
      named.prefix = true;
      if (auto again = request_network(*s.platform, named)) s.hotspot_request = *again;
    }
    if (!pause(s, 1.5)) return false;
  }
  fail(s,
       name_of(d) +
           "'s setup hotspot did not appear. A Chromecast starts it after failing to reach its "
           "Wi-Fi for a while; the TV then shows its setup screen.",
       false);
  return false;
}

void scan(Session& s) {
  Device& d = s.work.device;
  s.work.stage = Stage::kScanning;
  s.work.networks.clear();
  publish(s);
  auto started = post(d.endpoint, "/setup/scan_wifi");
  if (!started && !started.error().connected) {
    fail(s, "Lost contact with " + name_of(d) + ".", false);
    return;
  }
  std::vector<WifiNetwork> networks;
  for (int attempt = 0; attempt < 6 && networks.empty(); ++attempt) {
    if (!pause(s, attempt ? 2 : 3)) return;
    auto results = request(d.endpoint, "GET", "/setup/scan_results");
    if (results && results->status == 200) networks = parse_scan(results->body);
  }
  s.work.networks = std::move(networks);
  s.work.message = s.work.networks.empty() ? "The Chromecast found no Wi-Fi networks." : "";
  s.work.stage = Stage::kNetworks;
  publish(s);
}

// List the networks a device sees, joining its setup hotspot when it is out of
// reach. A hotspot the phone already joined is kept.
void open(Session& s, Device d) {
  s.work.device = d;
  s.work.networks.clear();
  s.work.target.clear();
  s.work.message.clear();
  s.work.retry = false;
  s.work.stage = Stage::kScanning;
  publish(s);
  bool reachable = false;
  if (d.reachable) {
    Endpoint e = d.endpoint;
    if (auto info = read_info(s, e, 3000); info && same_device(*info, d.info)) {
      d.info = *info;
      d.endpoint = e;
      reachable = true;
    }
  }
  if (interrupted(s)) return;
  if (s.target_request >= 0) release_network(*s.platform, s.target_request);
  s.target_request = -1;
  if (!reachable) {
    release_requests(s);
    d.reachable = d.hotspot = false;
    if (!join_hotspot(s, d)) return;
  } else if (!d.hotspot && s.hotspot_request >= 0) {
    release_network(*s.platform, s.hotspot_request);
    s.hotspot_request = -1;
  }
  s.work.device = d;
  scan(s);
}

void run_open(Session& s, int index) {
  Device d;
  if (index >= 0 && std::size_t(index) < s.work.devices.size()) d = s.work.devices[index];
  open(s, d);
}

// Persist the joined network on the device and confirm it is saved.
void finish(Session& s, Device& d, const WifiNetwork& target) {
  s.work.stage = Stage::kSaving;
  s.work.device = d;
  publish(s);
  bool saved = false;
  for (int attempt = 0; attempt < 4 && !saved; ++attempt) {
    if (attempt && !pause(s, 2)) return;
    auto configured = request(d.endpoint, "GET", "/setup/configured_networks");
    if (configured && configured->status == 200) {
      auto list = parse_configured(configured->body);
      if (std::find(list.begin(), list.end(), target.ssid) != list.end()) {
        saved = true;
        break;
      }
    }
    auto response = post(d.endpoint, "/setup/save_wifi");
    if (response && response->status == 200)
      if (auto state = parse_info(response->body); state && state->state == kSaved) saved = true;
  }
  if (interrupted(s)) return;
  Endpoint e = d.endpoint;
  if (auto info = read_info(s, e, 3000); info && same_device(*info, d.info)) d.info = *info;
  remember(s, d.info);
  save_known(s);
  release_requests(s);
  s.work.device = d;
  s.work.stage = Stage::kDone;
  bool home =
      !d.info.ip.empty() && !in_subnet(parse_address(d.info.ip), s.config.hotspot_address, 24);
  std::string where = home ? " at " + d.info.ip : "";
  s.work.message = name_of(d) + " is on " + target.ssid + where + ".";
  if (!saved) s.work.message += " It did not confirm saving the network.";
  publish(s);
}

void run_join(Session& s, int index, std::string password) {
  if (index < 0 || std::size_t(index) >= s.work.networks.size()) return;
  WifiNetwork target = s.work.networks[index];
  Device d = s.work.device;
  s.work.target = target.ssid;
  s.work.message.clear();
  s.work.retry = false;
  s.work.stage = Stage::kSending;
  publish(s);
  if (!supported(target)) {
    fail(s, "The Chromecast cannot join this network's type of security.", false);
    return;
  }
  if (!needs_password(target)) password.clear();
  else if (!valid_passphrase(password)) {
    fail(s, "Wi-Fi passwords have 8 to 63 characters.", true);
    return;
  }
  Endpoint e = d.endpoint;
  auto info = read_info(s, e, 4000);
  if (!info || (identified(d.info) && !same_device(*info, d.info))) {
    fail(s, "Lost contact with " + name_of(d) + ".", false);
    return;
  }
  d.info = *info;
  d.endpoint = e;
  std::string previous = d.hotspot || d.info.ssid.empty() ? "its network" : d.info.ssid;
  auto encrypted = encrypt_password(d.info.public_key, password);
  if (!encrypted) {
    fail(s, encrypted.error().message, false);
    return;
  }
  bool keep = d.hotspot && d.info.keeps_hotspot;
  auto sent = post(e, "/setup/connect_wifi", connect_body(target, *encrypted, keep));
  if (sent && sent->status == 400 && keep)
    sent = post(e, "/setup/connect_wifi", connect_body(target, *encrypted, false));
  if (sent && sent->status != 200) {
    fail(s, "The Chromecast refused the network (HTTP " + std::to_string(sent->status) + ").",
         false);
    return;
  }
  if (!sent && !sent.error().sent) {
    fail(s, "Could not send the network to " + name_of(d) + ": " + sent.error().message, false);
    return;
  }
  // A dropped connection means it already left to join the new network.
  bool left = !sent;
  s.work.stage = Stage::kJoining;
  s.work.device = d;
  publish(s);
  double start = monotonic();
  int misses = 0;
  while (since(s, start) < 40) {
    if (!pause(s, 2)) return;
    Endpoint probe = d.endpoint;
    auto now = read_info(s, probe, 2500);
    if (!now) {
      left = true;
      if (++misses >= 3) break;
      continue;
    }
    misses = 0;
    if (!same_device(*now, d.info)) continue;
    d.info = *now;
    d.endpoint = probe;
    if (failed_state(now->state)) {
      fail(s,
           name_of(d) + " could not join " + target.ssid + ": " + describe_state(now->state) + ".",
           now->state == kWrongPassword || now->state == kWrongPassword + 1);
      return;
    }
    if (now->ssid == target.ssid && now->state >= kConnected && now->state <= 64) {
      finish(s, d, target);
      return;
    }
    if (now->state >= 20 && now->state < kConnected) left = true;
  }
  if (!left) {
    fail(s,
         name_of(d) + " stayed on " + previous + ". Check the password and that " + target.ssid +
             " is in range.",
         true);
    return;
  }
  // Its hotspot is gone; look for it on the phone's other networks.
  if (s.hotspot_request >= 0) release_network(*s.platform, s.hotspot_request);
  s.hotspot_request = -1;
  s.work.stage = Stage::kFinding;
  publish(s);
  double finding = monotonic();
  while (since(s, finding) < 180) {
    std::vector<std::uint32_t> hints = {parse_address(d.info.ip)};
    for (const PhoneNetwork& n : list_networks(*s.platform)) {
      if (interrupted(s)) return;
      if (on_hotspot(s, n)) continue;
      for (Device& found : find_devices(s, n, hints)) {
        if (!same_device(found.info, d.info)) continue;
        int state = found.info.state;
        if (found.info.ssid == target.ssid && state >= kConnected && state <= 64) {
          found.info.public_key = d.info.public_key;
          d = found;
          finish(s, d, target);
          return;
        }
        if (failed_state(state)) {
          fail(s,
               name_of(d) + " could not join " + target.ssid + ": " + describe_state(state) + ".",
               true);
          return;
        }
        if (!found.info.ssid.empty() && state >= kConnected) {
          fail(s,
               name_of(d) + " could not join " + target.ssid + " and went back to " +
                   found.info.ssid + ". Check the password and that the network is in range.",
               true);
          return;
        }
      }
    }
    // The phone may not be on the new network; ask Android to join it too.
    if (s.target_request < 0 && since(s, finding) > 15) {
      WifiRequest r;
      r.ssid = target.ssid;
      r.passphrase = password;
      r.wpa3 = target.auth == 10;
      if (auto requested = request_network(*s.platform, r)) {
        s.target_request = *requested;
        s.work.prompt = true;
        publish(s);
      }
    }
    if (!pause(s, 2)) return;
  }
  fail(s,
       "Could not find " + name_of(d) + " on " + target.ssid +
           ". It may still be connecting or installing an update. Search again in a minute.",
       false);
}

void* run(void* data) {
  auto& s = *static_cast<Session*>(data);
  pthread_setname_np(pthread_self(), "chromecast");
  while (true) {
    pthread_mutex_lock(&s.mutex);
    while (s.command == Command::kNone) pthread_cond_wait(&s.wake, &s.mutex);
    Command command = s.command;
    int argument = s.argument;
    std::string password;
    password.swap(s.password);
    s.current = s.pending;
    s.command = Command::kNone;
    s.interrupted.store(false, std::memory_order_relaxed);
    pthread_mutex_unlock(&s.mutex);
    switch (command) {
      case Command::kSearch:
        run_search(s);
        break;
      case Command::kOpen:
        run_open(s, argument);
        break;
      case Command::kRescan:
        open(s, s.work.device);
        break;
      case Command::kJoin:
        run_join(s, argument, password);
        std::fill(password.begin(), password.end(), '\0');
        break;
      default:
        break;
    }
    if (command == Command::kStop) break;
  }
  release_requests(s);
  return nullptr;
}

std::uint64_t send(Session& s, Command command, int argument = -1, std::string password = {}) {
  pthread_mutex_lock(&s.mutex);
  std::uint64_t number = ++s.sequence;
  if (s.command != Command::kStop) {
    s.pending = number;
    std::fill(s.password.begin(), s.password.end(), '\0');
    s.command = command;
    s.argument = argument;
    s.password = std::move(password);
    s.interrupted.store(true, std::memory_order_relaxed);
    pthread_cond_signal(&s.wake);
  }
  pthread_mutex_unlock(&s.mutex);
  return number;
}

auto failure(const char* message) { return std::unexpected(common::Error{message}); }
}

common::Result<common::Owner<Session>> create_session(Platform& platform,
                                                      const SessionConfig& config) {
  common::Owner<Session> s(new (std::nothrow) Session);
  if (!s) return failure("Cannot allocate the session");
  s->platform = &platform;
  s->config = config;
  if (!(s->config.time_scale > 0)) s->config.time_scale = 1;
  pthread_condattr_t attributes;
  pthread_condattr_init(&attributes);
  pthread_condattr_setclock(&attributes, CLOCK_MONOTONIC);
  s->wake_ready = pthread_cond_init(&s->wake, &attributes) == 0;
  pthread_condattr_destroy(&attributes);
  if (!s->wake_ready) return failure("Cannot create the session's condition variable");
  s->fd = eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK);
  if (s->fd < 0) return failure("Cannot create the session's notification channel");
  auto saved = load_file(s->config.directory.c_str(), kDevicesFile);
  if (saved) {
    std::string_view text(reinterpret_cast<const char*>(saved->data()), saved->size());
    s->known = decode_known(text);
  }
  s->shared.stage = Stage::kSearching;
  if (pthread_create(&s->thread, nullptr, run, s.get()) != 0)
    return failure("Cannot start the session's worker thread");
  s->started = true;
  return s;
}

void destroy(Session* s) noexcept {
  if (s->started) {
    pthread_mutex_lock(&s->mutex);
    s->command = Command::kStop;
    s->interrupted.store(true, std::memory_order_relaxed);
    pthread_cond_signal(&s->wake);
    pthread_mutex_unlock(&s->mutex);
    pthread_join(s->thread, nullptr);
  }
  std::fill(s->password.begin(), s->password.end(), '\0');
  if (s->fd >= 0) close(s->fd);
  if (s->wake_ready) pthread_cond_destroy(&s->wake);
  pthread_mutex_destroy(&s->mutex);
  delete s;
}

int session_fd(const Session& s) { return s.fd; }

Snapshot take_snapshot(Session& s) {
  std::uint64_t count;
  (void)!read(s.fd, &count, sizeof(count));
  pthread_mutex_lock(&s.mutex);
  Snapshot copy = s.shared;
  pthread_mutex_unlock(&s.mutex);
  return copy;
}

std::uint64_t search(Session& s) { return send(s, Command::kSearch); }

std::uint64_t open_device(Session& s, int index) { return send(s, Command::kOpen, index); }

std::uint64_t rescan(Session& s) { return send(s, Command::kRescan); }

std::uint64_t join(Session& s, int network, std::string password) {
  return send(s, Command::kJoin, network, std::move(password));
}
}
