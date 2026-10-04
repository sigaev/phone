#include <arpa/inet.h>
#include <mbedtls/base64.h>
#include <mbedtls/ctr_drbg.h>
#include <mbedtls/entropy.h>
#include <mbedtls/pk.h>
#include <mbedtls/rsa.h>
#include <netinet/in.h>
#include <pthread.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cstdio>
#include <string>

#include "chromecast/discovery.h"
#include "chromecast/http.h"
#include "chromecast/json.h"
#include "chromecast/protocol.h"

namespace {
using namespace chromecast;

int failures = 0;

void check(bool condition, const char* message) {
  if (!condition) {
    std::fprintf(stderr, "FAILED: %s\n", message);
    ++failures;
  }
}

// Shaped like a real anchovy Chromecast's reply, with synthetic values.
constexpr char kInfo[] = R"({"build_info":{"build_type":2,"cast_build_revision":"1.36.159268"},
  "device_info":{"capabilities":{"keep_hotspot_until_connected_supported":true,
  "wifi_supported":true},"hotspot_bssid":"FA:8F:CA:00:00:01","mac_address":"6C:AD:F8:00:00:01",
  "model_name":"Chromecast","product_name":"anchovy","public_key":"MIIBCgKCAQEA",
  "ssdp_udn":"38002ef2-0000-0000-0000-000000000001","uptime":657.27},
  "name":"Living Room \u00e9","net":{"ethernet_connected":false,"ip_address":"192.168.1.80",
  "online":true},"setup":{"setup_state":60,"ssid_suffix":"","tos_accepted":true},"version":8,
  "wifi":{"bssid":"4a:e8:b7:00:00:01","signal_level":-51,"ssid":"Home","wpa_id":0}})";

constexpr char kScan[] = R"([
  {"ap_list":[{"bssid":"aa","frequency":2442,"signal_level":-35}],"bssid":"aa",
   "signal_level":-35,"ssid":"Neighbor","wpa_auth":7,"wpa_cipher":4},
  {"bssid":"bb","signal_level":-70,"ssid":"Home","wpa_auth":7,"wpa_cipher":4,"wpa_id":0},
  {"bssid":"cc","signal_level":-50,"ssid":"Home","wpa_auth":7,"wpa_cipher":4},
  {"bssid":"dd","signal_level":-60,"ssid":"Cafe","wpa_auth":1,"wpa_cipher":1},
  {"bssid":"ee","signal_level":-40,"ssid":"Office","wpa_auth":8,"wpa_cipher":4},
  {"bssid":"ff","signal_level":-30,"ssid":"","wpa_auth":7,"wpa_cipher":4}])";

void test_json() {
  auto value = parse_json(R"({"a":[1,2.5,-3e2],"b":{"c":"x\"\\\/\n\u00e9\ud83d\ude00"},"d":true,
    "e":false,"f":null})");
  check(value.has_value(), "JSON parses");
  if (!value) return;
  const Json* a = find(&*value, {"a"});
  check(a && a->items.size() == 3 && a->items[1].number == 2.5 && a->items[2].number == -300,
        "JSON arrays and numbers");
  check(as_string(find(&*value, {"b", "c"})) == "x\"\\/\n\xc3\xa9\xf0\x9f\x98\x80",
        "JSON escapes decode to UTF-8");
  check(as_bool(find(&*value, {"d"})) == true && as_bool(find(&*value, {"e"})) == false,
        "JSON booleans");
  check(find(&*value, {"f"})->type == Json::Type::kNull, "JSON null");
  check(!find(&*value, {"b", "missing"}) && !find(&*value, {"a", "c"}), "missing keys");
  check(as_integer(find(&*value, {"a"})) == std::nullopt, "arrays are not integers");
  for (const char* bad :
       {"", "{", "[1,]", "{\"a\" 1}", "01", "\"\\x\"", "tru", "[1] 2", "\"\\ud800\"", "1e999"})
    check(!parse_json(bad), "malformed JSON is rejected");
  std::string deep(40, '['), closing(40, ']');
  check(!parse_json(deep + closing), "deep nesting is rejected");
  check(parse_json(std::string(20, '[') + std::string(20, ']')).has_value(),
        "moderate nesting parses");
  check(quote("a\"b\\c\n\x01") == "\"a\\\"b\\\\c\\n\\u0001\"", "strings are quoted");
  auto round = parse_json(quote("tab\there"));
  check(round && round->string == "tab\there", "quoted strings parse back");
}

void test_info() {
  auto info = parse_info(kInfo);
  check(info.has_value(), "device info parses");
  if (!info) return;
  check(info->name == "Living Room \xc3\xa9" && info->model == "Chromecast" &&
            info->build == "1.36.159268",
        "device names");
  check(info->mac == "6C:AD:F8:00:00:01" && info->hotspot_bssid == "FA:8F:CA:00:00:01" &&
            info->udn == "38002ef2-0000-0000-0000-000000000001",
        "device identity");
  check(info->ssid == "Home" && info->ip == "192.168.1.80" && info->state == 60 &&
            info->signal == -51 && info->keeps_hotspot && info->public_key == "MIIBCgKCAQEA",
        "device network state");
  auto flat = parse_info(R"({"name":"Old","setup_state":61,"ssid":"Net","ip_address":"10.0.0.2",
    "public_key":"KEY","mac_address":"aa"})");
  check(flat && flat->state == 61 && flat->ssid == "Net" && flat->ip == "10.0.0.2" &&
            flat->public_key == "KEY" && flat->mac == "aa",
        "older flat device info parses");
  check(!parse_info("{\"other\":1}") && !parse_info("[]") && !parse_info("nonsense"),
        "other services are not devices");
  DeviceInfo a = *info, b = *info;
  b.mac = "6c:ad:f8:00:00:01";
  b.name = "Renamed";
  check(same_device(a, b), "MAC addresses compare without case");
  b.mac = "6C:AD:F8:00:00:02";
  check(!same_device(a, b), "different MAC addresses differ");
  b.mac.clear();
  check(same_device(a, b), "UDNs identify devices without a MAC");
  check(std::string(describe_state(31)) == "Wrong password" && failed_state(31) &&
            failed_state(21) && !failed_state(60) && !failed_state(30),
        "setup states");
}

void test_scan() {
  auto networks = parse_scan(kScan);
  check(networks.size() == 4, "duplicates and hidden networks are merged or dropped");
  if (networks.size() != 4) return;
  check(networks[0].ssid == "Neighbor" && networks[1].ssid == "Office" &&
            networks[2].ssid == "Home" && networks[3].ssid == "Cafe",
        "networks sort by signal");
  check(networks[2].signal == -50 && networks[2].bssid == "cc" && networks[2].wpa_id == 0,
        "the strongest duplicate is kept with the saved id");
  check(networks[0].frequency == 2442, "frequencies come from the access point list");
  check(!supported(networks[1]) && supported(networks[0]) && supported(networks[3]),
        "enterprise networks are unsupported");
  check(!needs_password(networks[3]) && needs_password(networks[0]), "open networks");
  auto wrapped = parse_scan(R"({"networks":[{"ssid":"A","wpa_auth":7,"wpa_cipher":4}]})");
  check(wrapped.size() == 1 && wrapped[0].ssid == "A", "wrapped scan results");
  auto saved = parse_configured(R"([{"ssid":"Home","wpa_auth":7,"wpa_id":0},{"ssid":"B"}])");
  check(saved.size() == 2 && saved[0] == "Home" && saved[1] == "B", "configured networks");
  check(valid_passphrase("12345678") && valid_passphrase(std::string(63, 'x')) &&
            valid_passphrase(std::string(64, 'a')) && !valid_passphrase(std::string(64, 'g')) &&
            !valid_passphrase("1234567") && !valid_passphrase(std::string(65, 'a')) &&
            !valid_passphrase("caf\xc3\xa9 1234"),
        "WPA passphrase rules");
}

struct Keys {
  mbedtls_entropy_context entropy;
  mbedtls_ctr_drbg_context drbg;
  mbedtls_pk_context pk;
};

std::string base64(const unsigned char* data, std::size_t size) {
  unsigned char out[2048];
  std::size_t length = 0;
  mbedtls_base64_encode(out, sizeof(out), &length, data, size);
  return std::string(reinterpret_cast<char*>(out), length);
}

void test_encryption() {
  Keys k;
  mbedtls_entropy_init(&k.entropy);
  mbedtls_ctr_drbg_init(&k.drbg);
  mbedtls_pk_init(&k.pk);
  bool made =
      mbedtls_ctr_drbg_seed(&k.drbg, mbedtls_entropy_func, &k.entropy, nullptr, 0) == 0 &&
      mbedtls_pk_setup(&k.pk, mbedtls_pk_info_from_type(MBEDTLS_PK_RSA)) == 0 &&
      mbedtls_rsa_gen_key(mbedtls_pk_rsa(k.pk), mbedtls_ctr_drbg_random, &k.drbg, 1024, 65537) == 0;
  check(made, "test RSA key");
  if (made) {
    unsigned char der[1024];
    unsigned char* end = der + sizeof(der);
    int pkcs1 = mbedtls_pk_write_pubkey(&end, der, &k.pk);
    std::string pkcs1_key = base64(end, pkcs1);
    int spki = mbedtls_pk_write_pubkey_der(&k.pk, der, sizeof(der));
    std::string spki_key = base64(der + sizeof(der) - spki, spki);
    for (const std::string& key : {pkcs1_key, spki_key}) {
      // Line breaks in the key are ignored.
      std::string broken = key.substr(0, 20) + "\n" + key.substr(20);
      auto encrypted = encrypt_password(broken, "correct horse");
      check(encrypted.has_value(), "passwords encrypt with PKCS#1 and SPKI keys");
      if (!encrypted) continue;
      unsigned char cipher[256], plain[256];
      std::size_t size = 0, length = 0;
      bool decoded =
          mbedtls_base64_decode(cipher, sizeof(cipher), &size,
                                reinterpret_cast<const unsigned char*>(encrypted->data()),
                                encrypted->size()) == 0 &&
          mbedtls_rsa_pkcs1_decrypt(mbedtls_pk_rsa(k.pk), mbedtls_ctr_drbg_random, &k.drbg, &length,
                                    cipher, plain, sizeof(plain)) == 0;
      check(decoded && size == 128 &&
                std::string(reinterpret_cast<char*>(plain), length) == "correct horse",
            "the device's private key decrypts the password");
      auto again = encrypt_password(key, "correct horse");
      check(again && *again != *encrypted, "PKCS#1 v1.5 padding is random");
    }
    auto open = encrypt_password(pkcs1_key, "");
    check(open.has_value(), "empty passwords of open networks encrypt");
  }
  check(!encrypt_password("", "x") && !encrypt_password("!!!!", "x") &&
            !encrypt_password("AAAA", "x"),
        "invalid keys are rejected");
  mbedtls_pk_free(&k.pk);
  mbedtls_ctr_drbg_free(&k.drbg);
  mbedtls_entropy_free(&k.entropy);

  WifiNetwork n;
  n.ssid = "Caf\"e";
  n.auth = 7;
  n.cipher = 4;
  auto body = parse_json(connect_body(n, "ENC", false));
  check(body && as_string(find(&*body, {"ssid"})) == "Caf\"e" &&
            as_integer(find(&*body, {"wpa_auth"})) == 7 &&
            as_integer(find(&*body, {"wpa_cipher"})) == 4 &&
            as_integer(find(&*body, {"wpa_id"})) == 0 &&
            as_integer(find(&*body, {"scan_ssid"})) == 0 &&
            as_string(find(&*body, {"enc_passwd"})) == "ENC" &&
            !find(&*body, {"keep_hotspot_until_connected"}) && !find(&*body, {"passwd"}),
        "connect_wifi body");
  n.wpa_id = 3;
  auto kept = parse_json(connect_body(n, "ENC", true));
  check(kept && as_integer(find(&*kept, {"wpa_id"})) == 3 &&
            as_bool(find(&*kept, {"keep_hotspot_until_connected"})) == true,
        "connect_wifi keeps the hotspot when asked");
}

int bound_socket(int type, std::uint32_t address, std::uint16_t& port) {
  int fd = socket(AF_INET, type | SOCK_CLOEXEC, 0);
  sockaddr_in where{};
  where.sin_family = AF_INET;
  where.sin_addr.s_addr = htonl(address);
  socklen_t size = sizeof(where);
  if (fd < 0 || bind(fd, reinterpret_cast<sockaddr*>(&where), size) != 0 ||
      getsockname(fd, reinterpret_cast<sockaddr*>(&where), &size) != 0)
    return -1;
  if (type == SOCK_STREAM) listen(fd, 8);
  port = ntohs(where.sin_port);
  return fd;
}

struct Responder {
  int fd;
  bool named;
};

// Answers one mDNS question like a Cast device, with or without an A record.
void* respond(void* data) {
  auto& r = *static_cast<Responder*>(data);
  unsigned char query[512];
  sockaddr_in from{};
  socklen_t size = sizeof(from);
  ssize_t n = recvfrom(r.fd, query, sizeof(query), 0, reinterpret_cast<sockaddr*>(&from), &size);
  if (n < 12) return nullptr;
  // Header, the echoed question, then a PTR and an A record using compression.
  std::string reply(reinterpret_cast<char*>(query), 2);
  reply += std::string("\x84\x00\x00\x01\x00\x01\x00\x00\x00", 9) + char(r.named ? 1 : 0);
  reply.append(reinterpret_cast<char*>(query) + 12, n - 12);
  reply += std::string("\xc0\x0c\x00\x0c\x00\x01\x00\x00\x00\x78\x00\x02\xc0\x0c", 14);
  if (r.named)
    reply += std::string("\xc0\x0c\x00\x01\x80\x01\x00\x00\x00\x78\x00\x04\x7f\x00\x00\x09", 16);
  sendto(r.fd, reply.data(), reply.size(), 0, reinterpret_cast<sockaddr*>(&from), size);
  return nullptr;
}

void test_discovery() {
  for (bool named : {true, false}) {
    std::uint16_t port = 0;
    Responder r{bound_socket(SOCK_DGRAM, 0x7f000001, port), named};
    check(r.fd >= 0, "mDNS responder socket");
    if (r.fd < 0) continue;
    pthread_t thread;
    pthread_create(&thread, nullptr, respond, &r);
    auto found = query_mdns(0, 0x7f000002, 0x7f000001, port, 600);
    pthread_join(thread, nullptr);
    close(r.fd);
    std::uint32_t expected = named ? 0x7f000009 : 0x7f000001;
    check(found.size() == 1 && found[0] == expected,
          named ? "mDNS A records name devices" : "mDNS senders are devices");
  }
  std::uint16_t port = 0;
  int listener = bound_socket(SOCK_STREAM, 0x7f000001, port);
  auto open = sweep_subnet(0, 0x7f000002, 16, port, 800);
  check(open.size() == 1 && open[0] == 0x7f000001, "the subnet sweep finds an open port");
  check(port_open({0, 0x7f000001, port, false}, 500), "open ports accept connections");
  close(listener);
  check(!port_open({0, 0x7f000001, port, false}, 500), "closed ports refuse connections");
  auto refused = request({0, 0x7f000001, port, true}, "GET", kInfoPath, {}, 500);
  check(!refused && !refused.error().connected && !refused.error().sent,
        "refused connections are reported");
  check(format_address(0xc0a8fff9) == "192.168.255.249", "addresses format");
}
}

int main() {
  test_json();
  test_info();
  test_scan();
  test_encryption();
  test_discovery();
  if (failures) return 1;
  std::printf("protocol_test passed\n");
  return 0;
}
