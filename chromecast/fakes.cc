#include "chromecast/fakes.h"

#include <arpa/inet.h>
#include <mbedtls/base64.h>
#include <mbedtls/ctr_drbg.h>
#include <mbedtls/ecp.h>
#include <mbedtls/entropy.h>
#include <mbedtls/net_sockets.h>
#include <mbedtls/pk.h>
#include <mbedtls/rsa.h>
#include <mbedtls/ssl.h>
#include <mbedtls/x509_crt.h>
#include <netinet/in.h>
#include <poll.h>
#include <psa/crypto.h>
#include <pthread.h>
#include <sys/eventfd.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <cstdlib>
#include <new>

#include "chromecast/http.h"
#include "chromecast/json.h"

namespace chromecast {
namespace {
constexpr char kHotspot[] = "#hotspot";

double monotonic() {
  timespec now;
  clock_gettime(CLOCK_MONOTONIC, &now);
  return double(now.tv_sec) + now.tv_nsec * 1e-9;
}

auto failure(const char* message) { return std::unexpected(common::Error{message}); }
}

struct FakeDevice {
  FakeDeviceConfig config;
  pthread_mutex_t mutex = PTHREAD_MUTEX_INITIALIZER;
  pthread_t thread{};
  bool started = false, crypto = false;
  int stop = -1;
  // Home-network HTTPS and HTTP, then hotspot HTTPS and HTTP.
  int listeners[4] = {-1, -1, -1, -1};
  std::uint16_t https = 0, http = 0;
  mbedtls_entropy_context entropy;
  mbedtls_ctr_drbg_context drbg;
  mbedtls_pk_context tls_key, device_key;
  mbedtls_x509_crt certificate;
  mbedtls_ssl_config tls;
  std::string public_key;
  // Guarded by mutex.
  bool setup_mode = false;
  // Joined a network while keeping its hotspot up.
  bool joined_on_hotspot = false;
  std::string ssid;
  int state = 10;
  double offline_until = 0;
  std::vector<std::string> phone;
  FakeRecord record;
};

namespace {
bool has(const std::vector<std::string>& list, const std::string& value) {
  return std::find(list.begin(), list.end(), value) != list.end();
}

// Whether the listener at index answers, given where the device is.
bool reachable(FakeDevice& d, int listener) {
  if (monotonic() < d.offline_until) return false;
  if (listener < 2) return !d.setup_mode && !d.ssid.empty() && has(d.phone, d.ssid);
  return d.setup_mode && has(d.phone, kHotspot);
}

const FakeNetwork* network(const FakeDevice& d, const std::string& ssid) {
  for (const FakeNetwork& n : d.config.networks)
    if (n.ssid == ssid) return &n;
  return nullptr;
}

std::string info_json(FakeDevice& d) {
  bool home = !d.ssid.empty();
  std::string ip = home ? format_address(d.config.lan) : "";
  return "{\"name\":" + quote(d.config.name) +
         ",\"build_info\":{\"cast_build_revision\":\"1.36.159268\"},\"device_info\":{"
         "\"capabilities\":{\"keep_hotspot_until_connected_supported\":" +
         (d.config.keeps_hotspot ? "true" : "false") +
         "},\"hotspot_bssid\":" + quote(d.config.hotspot_bssid) +
         ",\"mac_address\":" + quote(d.config.mac) +
         ",\"model_name\":\"Chromecast\",\"public_key\":" + quote(d.public_key) +
         ",\"ssdp_udn\":\"00000000-0000-0000-0000-" + d.config.mac.substr(0, 2) +
         "\"},\"net\":{\"ip_address\":" + quote(ip) +
         "},\"setup\":{\"setup_state\":" + std::to_string(d.state) +
         "},\"wifi\":{\"ssid\":" + quote(d.ssid) + ",\"signal_level\":-48}}";
}

std::string scan_json(FakeDevice& d) {
  std::string out = "[";
  int signal = -40;
  for (const FakeNetwork& n : d.config.networks) {
    if (out.size() > 1) out += ",";
    out += "{\"ssid\":" + quote(n.ssid) +
           ",\"bssid\":\"00:11:22:33:44:55\",\"signal_level\":" + std::to_string(signal) +
           ",\"wpa_auth\":" + std::to_string(n.auth) +
           ",\"wpa_cipher\":" + (n.auth == 1 ? "1" : "4");
    auto saved = std::find(d.record.configured.begin(), d.record.configured.end(), n.ssid);
    if (saved != d.record.configured.end())
      out += ",\"wpa_id\":" + std::to_string(saved - d.record.configured.begin());
    out += "}";
    signal -= 9;
  }
  return out + "]";
}

std::string configured_json(FakeDevice& d) {
  std::string out = "[";
  for (std::size_t i = 0; i < d.record.configured.size(); ++i) {
    if (i) out += ",";
    out += "{\"ssid\":" + quote(d.record.configured[i]) + ",\"wpa_id\":" + std::to_string(i) + "}";
  }
  return out + "]";
}

std::string decrypt(FakeDevice& d, const std::string& base64) {
  unsigned char cipher[512], plain[512];
  std::size_t size = 0, length = 0;
  if (mbedtls_base64_decode(cipher, sizeof(cipher), &size,
                            reinterpret_cast<const unsigned char*>(base64.data()),
                            base64.size()) != 0 ||
      mbedtls_rsa_pkcs1_decrypt(mbedtls_pk_rsa(d.device_key), mbedtls_ctr_drbg_random, &d.drbg,
                                &length, cipher, plain, sizeof(plain)) != 0)
    return "\x01undecryptable";
  return std::string(reinterpret_cast<char*>(plain), length);
}

// Handles one request with the mutex held; returns status and body.
std::pair<int, std::string> handle(FakeDevice& d, std::string_view method, std::string_view path,
                                   std::string_view content_type, std::string_view body) {
  if (method == "GET" && path.starts_with("/setup/eureka_info")) return {200, info_json(d)};
  if (method == "GET" && path == "/setup/scan_results") {
    if (d.config.scan == FakeScanBehavior::kEmpty) return {200, "[]"};
    if (d.config.scan == FakeScanBehavior::kInvalid) return {200, "{}"};
    return {200, scan_json(d)};
  }
  if (method == "GET" && path == "/setup/configured_networks") return {200, configured_json(d)};
  if (method == "POST" && path == "/setup/scan_wifi") {
    if (d.config.scan == FakeScanBehavior::kReject) return {403, ""};
    // The hotspot vanished after acknowledging the scan, as with an expired
    // Android PendingIntent request. The scan itself would have found networks.
    if (d.config.scan == FakeScanBehavior::kDisconnect) d.offline_until = monotonic() + 60;
    return {200, ""};
  }
  if (method == "POST" && path == "/setup/save_wifi") {
    ++d.record.saves;
    if (d.state == 61) {
      if (!has(d.record.configured, d.ssid)) d.record.configured.push_back(d.ssid);
      d.state = 60;
      // The hotspot closes once the network is saved.
      if (d.joined_on_hotspot) d.setup_mode = d.joined_on_hotspot = false;
    }
    return {200, "{\"setup_state\":" + std::to_string(d.state) + "}"};
  }
  if (method == "POST" && path == "/setup/connect_wifi") {
    // Like real devices, reject a charset parameter.
    if (content_type != "application/json") return {400, ""};
    auto parsed = parse_json(body);
    auto target = parsed ? as_string(find(&*parsed, {"ssid"})) : std::nullopt;
    auto encrypted = parsed ? as_string(find(&*parsed, {"enc_passwd"})) : std::nullopt;
    if (!target || !encrypted) return {400, ""};
    bool keep = parsed && as_bool(find(&*parsed, {"keep_hotspot_until_connected"})).value_or(false);
    std::string password = decrypt(d, *encrypted);
    ++d.record.connects;
    d.record.last_ssid = *target;
    d.record.last_password = password;
    d.record.keep_hotspot_requested = keep;
    const FakeNetwork* n = network(d, *target);
    bool correct = n && n->password == password;
    if (d.setup_mode && keep && d.config.keeps_hotspot) {
      if (correct) {
        d.ssid = *target;
        d.state = 61;
        d.joined_on_hotspot = true;
      } else {
        d.state = 31;
      }
      return {200, ""};
    }
    d.offline_until = monotonic() + d.config.switch_seconds;
    if (correct) {
      d.setup_mode = false;
      d.ssid = *target;
      d.state = 61;
    } else if (!d.setup_mode) {
      d.state = 60;
    } else {
      d.state = 31;
    }
    return {200, ""};
  }
  return {404, ""};
}

struct Peer {
  int fd = -1;
  bool tls = false;
  mbedtls_ssl_context ssl;
};

void destroy(Peer* p) noexcept {
  if (p->tls) mbedtls_ssl_free(&p->ssl);
  if (p->fd >= 0) close(p->fd);
  delete p;
}

int peer_send(void* context, const unsigned char* data, std::size_t size) {
  ssize_t n = send(*static_cast<int*>(context), data, size, MSG_NOSIGNAL);
  return n >= 0           ? int(n)
         : errno == EINTR ? MBEDTLS_ERR_SSL_WANT_WRITE
                          : MBEDTLS_ERR_NET_SEND_FAILED;
}

int peer_receive(void* context, unsigned char* data, std::size_t size) {
  ssize_t n = recv(*static_cast<int*>(context), data, size, 0);
  return n >= 0 ? int(n) : errno == EINTR ? MBEDTLS_ERR_SSL_WANT_READ : MBEDTLS_ERR_NET_RECV_FAILED;
}

int peer_read(Peer& p, char* buffer, std::size_t size) {
  while (true) {
    int n = p.tls ? mbedtls_ssl_read(&p.ssl, reinterpret_cast<unsigned char*>(buffer), size)
                  : int(recv(p.fd, buffer, size, 0));
    if (p.tls && (n == MBEDTLS_ERR_SSL_WANT_READ || n == MBEDTLS_ERR_SSL_WANT_WRITE)) continue;
    if (!p.tls && n < 0 && errno == EINTR) continue;
    return n;
  }
}

void peer_write(Peer& p, const std::string& data) {
  for (std::size_t at = 0; at < data.size();) {
    auto* bytes = reinterpret_cast<const unsigned char*>(data.data() + at);
    int n = p.tls ? mbedtls_ssl_write(&p.ssl, bytes, data.size() - at)
                  : int(send(p.fd, bytes, data.size() - at, MSG_NOSIGNAL));
    if (p.tls && (n == MBEDTLS_ERR_SSL_WANT_READ || n == MBEDTLS_ERR_SSL_WANT_WRITE)) continue;
    if (n <= 0) return;
    at += n;
  }
}

void serve(FakeDevice& d, int fd, int listener) {
  common::Owner<Peer> p(new (std::nothrow) Peer);
  if (!p) {
    close(fd);
    return;
  }
  p->fd = fd;
  timeval limit{2, 0};
  setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &limit, sizeof(limit));
  setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &limit, sizeof(limit));
  pthread_mutex_lock(&d.mutex);
  bool up = reachable(d, listener);
  pthread_mutex_unlock(&d.mutex);
  // Out of reach: the connection opens, then closes without an answer.
  if (!up) return;
  if (listener % 2 == 0) {
    mbedtls_ssl_init(&p->ssl);
    p->tls = true;
    if (mbedtls_ssl_setup(&p->ssl, &d.tls) != 0) return;
    mbedtls_ssl_set_bio(&p->ssl, &p->fd, peer_send, peer_receive, nullptr);
    int result;
    do result = mbedtls_ssl_handshake(&p->ssl);
    while (result == MBEDTLS_ERR_SSL_WANT_READ || result == MBEDTLS_ERR_SSL_WANT_WRITE);
    if (result != 0) return;
  }
  std::string raw;
  char buffer[4096];
  std::size_t head = std::string::npos, length = 0;
  while (true) {
    if (head == std::string::npos && (head = raw.find("\r\n\r\n")) != std::string::npos) {
      head += 4;
      std::size_t at = raw.find("Content-Length: ");
      if (at != std::string::npos && at < head)
        length = std::strtoul(raw.c_str() + at + 16, nullptr, 10);
    }
    if (head != std::string::npos && raw.size() >= head + length) break;
    int n = peer_read(*p, buffer, sizeof(buffer));
    if (n <= 0) return;
    raw.append(buffer, n);
  }
  std::string_view request(raw);
  std::size_t space = request.find(' '), second = request.find(' ', space + 1);
  std::string_view method = request.substr(0, space);
  std::string_view path = request.substr(space + 1, second - space - 1);
  std::string_view type;
  if (std::size_t at = request.find("Content-Type: "); at < head) {
    type = request.substr(at + 14);
    type = type.substr(0, type.find("\r\n"));
  }
  pthread_mutex_lock(&d.mutex);
  auto [status, body] = handle(d, method, path, type, request.substr(head, length));
  pthread_mutex_unlock(&d.mutex);
  peer_write(*p, "HTTP/1.1 " + std::to_string(status) +
                     " OK\r\nContent-Type: application/json\r\nContent-Length: " +
                     std::to_string(body.size()) + "\r\n\r\n" + body);
  if (p->tls) mbedtls_ssl_close_notify(&p->ssl);
}

void* run(void* data) {
  auto& d = *static_cast<FakeDevice*>(data);
  while (true) {
    pollfd fds[5];
    for (int i = 0; i < 4; ++i) fds[i] = {d.listeners[i], POLLIN, 0};
    fds[4] = {d.stop, POLLIN, 0};
    if (poll(fds, 5, -1) < 0 && errno != EINTR) break;
    if (fds[4].revents) break;
    for (int i = 0; i < 4; ++i)
      if (fds[i].revents & POLLIN) {
        int fd = accept4(d.listeners[i], nullptr, nullptr, SOCK_CLOEXEC);
        if (fd >= 0) serve(d, fd, i);
      }
  }
  return nullptr;
}

int listen_on(std::uint32_t address, std::uint16_t& port) {
  int fd = socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, IPPROTO_TCP);
  if (fd < 0) return -1;
  int one = 1;
  setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
  sockaddr_in where{};
  where.sin_family = AF_INET;
  where.sin_port = htons(port);
  where.sin_addr.s_addr = htonl(address);
  socklen_t size = sizeof(where);
  if (bind(fd, reinterpret_cast<sockaddr*>(&where), size) != 0 || listen(fd, 64) != 0 ||
      getsockname(fd, reinterpret_cast<sockaddr*>(&where), &size) != 0) {
    close(fd);
    return -1;
  }
  port = ntohs(where.sin_port);
  return fd;
}

bool make_keys(FakeDevice& d) {
  auto rng = mbedtls_ctr_drbg_random;
  if (mbedtls_ctr_drbg_seed(&d.drbg, mbedtls_entropy_func, &d.entropy, nullptr, 0) != 0)
    return false;
  if (mbedtls_pk_setup(&d.device_key, mbedtls_pk_info_from_type(MBEDTLS_PK_RSA)) != 0 ||
      mbedtls_rsa_gen_key(mbedtls_pk_rsa(d.device_key), rng, &d.drbg, 1024, 65537) != 0)
    return false;
  unsigned char der[1024];
  unsigned char* end = der + sizeof(der);
  int size = mbedtls_pk_write_pubkey(&end, der, &d.device_key);
  if (size <= 0) return false;
  unsigned char encoded[2048];
  std::size_t length = 0;
  if (mbedtls_base64_encode(encoded, sizeof(encoded), &length, end, size) != 0) return false;
  d.public_key.assign(reinterpret_cast<char*>(encoded), length);
  if (mbedtls_pk_setup(&d.tls_key, mbedtls_pk_info_from_type(MBEDTLS_PK_ECKEY)) != 0 ||
      mbedtls_ecp_gen_key(MBEDTLS_ECP_DP_SECP256R1, mbedtls_pk_ec(d.tls_key), rng, &d.drbg) != 0)
    return false;
  mbedtls_x509write_cert writer;
  mbedtls_x509write_crt_init(&writer);
  mbedtls_x509write_crt_set_version(&writer, MBEDTLS_X509_CRT_VERSION_3);
  mbedtls_x509write_crt_set_md_alg(&writer, MBEDTLS_MD_SHA256);
  mbedtls_x509write_crt_set_subject_key(&writer, &d.tls_key);
  mbedtls_x509write_crt_set_issuer_key(&writer, &d.tls_key);
  static constexpr unsigned char kSerial[] = {1};
  bool written =
      mbedtls_x509write_crt_set_subject_name(&writer, "CN=Fake Chromecast") == 0 &&
      mbedtls_x509write_crt_set_issuer_name(&writer, "CN=Fake Chromecast") == 0 &&
      mbedtls_x509write_crt_set_serial_raw(&writer, const_cast<unsigned char*>(kSerial), 1) == 0 &&
      mbedtls_x509write_crt_set_validity(&writer, "20240101000000", "20400101000000") == 0;
  unsigned char certificate[4096];
  int bytes =
      written ? mbedtls_x509write_crt_der(&writer, certificate, sizeof(certificate), rng, &d.drbg)
              : -1;
  mbedtls_x509write_crt_free(&writer);
  if (bytes <= 0 || mbedtls_x509_crt_parse_der(
                        &d.certificate, certificate + sizeof(certificate) - bytes, bytes) != 0)
    return false;
  if (mbedtls_ssl_config_defaults(&d.tls, MBEDTLS_SSL_IS_SERVER, MBEDTLS_SSL_TRANSPORT_STREAM,
                                  MBEDTLS_SSL_PRESET_DEFAULT) != 0)
    return false;
  mbedtls_ssl_conf_rng(&d.tls, rng, &d.drbg);
  mbedtls_ssl_conf_max_tls_version(&d.tls, MBEDTLS_SSL_VERSION_TLS1_2);
  return mbedtls_ssl_conf_own_cert(&d.tls, &d.certificate, &d.tls_key) == 0;
}

void set_phone(FakeDevice& d, std::vector<std::string> ssids) {
  pthread_mutex_lock(&d.mutex);
  d.phone = std::move(ssids);
  pthread_mutex_unlock(&d.mutex);
}
}

common::Result<common::Owner<FakeDevice>> create_fake_device(const FakeDeviceConfig& config) {
  common::Owner<FakeDevice> d(new (std::nothrow) FakeDevice);
  if (!d) return failure("Cannot allocate the fake device");
  d->config = config;
  mbedtls_entropy_init(&d->entropy);
  mbedtls_ctr_drbg_init(&d->drbg);
  mbedtls_pk_init(&d->tls_key);
  mbedtls_pk_init(&d->device_key);
  mbedtls_x509_crt_init(&d->certificate);
  mbedtls_ssl_config_init(&d->tls);
  d->crypto = true;
  if (psa_crypto_init() != PSA_SUCCESS || !make_keys(*d))
    return failure("Cannot create the fake device's keys");
  d->setup_mode = config.setup_mode;
  if (!config.setup_mode && !config.networks.empty()) {
    d->ssid = config.networks[0].ssid;
    d->record.configured.push_back(d->ssid);
    d->state = 60;
  }
  std::uint16_t https = 0, http = 0;
  d->listeners[0] = listen_on(config.lan, https);
  d->listeners[1] = listen_on(config.lan, http);
  d->listeners[2] = listen_on(config.hotspot, https);
  d->listeners[3] = listen_on(config.hotspot, http);
  for (int fd : d->listeners)
    if (fd < 0) return failure("Cannot listen on the fake device's addresses");
  d->https = https;
  d->http = http;
  d->stop = eventfd(0, EFD_CLOEXEC);
  if (d->stop < 0 || pthread_create(&d->thread, nullptr, run, d.get()) != 0)
    return failure("Cannot start the fake device");
  d->started = true;
  return d;
}

void destroy(FakeDevice* d) noexcept {
  if (d->started) {
    const std::uint64_t one = 1;
    (void)!write(d->stop, &one, sizeof(one));
    pthread_join(d->thread, nullptr);
  }
  for (int fd : d->listeners)
    if (fd >= 0) close(fd);
  if (d->stop >= 0) close(d->stop);
  if (d->crypto) {
    mbedtls_ssl_config_free(&d->tls);
    mbedtls_x509_crt_free(&d->certificate);
    mbedtls_pk_free(&d->device_key);
    mbedtls_pk_free(&d->tls_key);
    mbedtls_ctr_drbg_free(&d->drbg);
    mbedtls_entropy_free(&d->entropy);
  }
  pthread_mutex_destroy(&d->mutex);
  delete d;
}

std::uint16_t https_port(const FakeDevice& d) { return d.https; }

std::uint16_t http_port(const FakeDevice& d) { return d.http; }

FakeRecord get_record(FakeDevice& d) {
  pthread_mutex_lock(&d.mutex);
  FakeRecord copy = d.record;
  pthread_mutex_unlock(&d.mutex);
  return copy;
}

std::string current_ssid(FakeDevice& d) {
  pthread_mutex_lock(&d.mutex);
  std::string copy = d.setup_mode && !d.joined_on_hotspot ? "" : d.ssid;
  pthread_mutex_unlock(&d.mutex);
  return copy;
}

struct Platform {
  FakeDevice* device = nullptr;
  FakeRequestBehavior behavior = FakeRequestBehavior::kConnect;
  pthread_mutex_t mutex = PTHREAD_MUTEX_INITIALIZER;

  struct Joined {
    std::string ssid;
    PhoneNetwork network;
    // The request that added it, or -1 for a network the phone started on.
    int request;
  };

  std::vector<Joined> networks;
  std::vector<WifiRequest> history;
  std::vector<int> active;
  int next = 1;
  std::uint32_t next_address = 0x7f000002;
};

namespace {
void sync(Platform& p) {
  std::vector<std::string> ssids;
  for (const auto& n : p.networks) ssids.push_back(n.ssid);
  set_phone(*p.device, std::move(ssids));
}
}

void destroy(Platform* p) noexcept {
  pthread_mutex_destroy(&p->mutex);
  delete p;
}

common::Owner<Platform> create_fake_platform(FakeDevice& device,
                                             const std::vector<std::string>& ssids,
                                             FakeRequestBehavior behavior) {
  common::Owner<Platform> p(new (std::nothrow) Platform);
  if (!p) return p;
  p->device = &device;
  p->behavior = behavior;
  for (const std::string& ssid : ssids)
    p->networks.push_back({ssid, {0, p->next_address++, 24}, -1});
  sync(*p);
  return p;
}

std::vector<PhoneNetwork> list_networks(Platform& p) {
  pthread_mutex_lock(&p.mutex);
  std::vector<PhoneNetwork> result;
  for (const auto& n : p.networks) result.push_back(n.network);
  pthread_mutex_unlock(&p.mutex);
  return result;
}

common::Result<int> request_network(Platform& p, const WifiRequest& r) {
  pthread_mutex_lock(&p.mutex);
  int id = p.next++;
  p.history.push_back(r);
  if (p.behavior == FakeRequestBehavior::kError) {
    pthread_mutex_unlock(&p.mutex);
    return failure("Wi-Fi request refused");
  }
  p.active.push_back(id);
  if (p.behavior == FakeRequestBehavior::kWait) {
    pthread_mutex_unlock(&p.mutex);
    return id;
  }
  FakeDevice& d = *p.device;
  pthread_mutex_lock(&d.mutex);
  bool hotspot = d.setup_mode && ((!r.bssid.empty() && r.bssid == d.config.hotspot_bssid) ||
                                  (r.prefix && d.config.name.starts_with(r.ssid)));
  const FakeNetwork* known = r.prefix ? nullptr : network(d, r.ssid);
  pthread_mutex_unlock(&d.mutex);
  // The person approves Android's prompt at once.
  if (hotspot) {
    std::uint32_t address = (d.config.hotspot & 0xffffff00) | 2;
    p.networks.push_back({kHotspot, {0, address, 24}, id});
  } else if (known && known->password == r.passphrase) {
    p.networks.push_back({r.ssid, {0, p.next_address++, 24}, id});
  }
  sync(p);
  pthread_mutex_unlock(&p.mutex);
  return id;
}

void release_network(Platform& p, int id) {
  pthread_mutex_lock(&p.mutex);
  std::erase_if(p.networks, [&](const Platform::Joined& n) { return n.request == id; });
  std::erase(p.active, id);
  sync(p);
  pthread_mutex_unlock(&p.mutex);
}

std::vector<WifiRequest> get_requests(Platform& p) {
  pthread_mutex_lock(&p.mutex);
  auto copy = p.history;
  pthread_mutex_unlock(&p.mutex);
  return copy;
}

int active_requests(Platform& p) {
  pthread_mutex_lock(&p.mutex);
  int count = int(p.active.size());
  pthread_mutex_unlock(&p.mutex);
  return count;
}
}
