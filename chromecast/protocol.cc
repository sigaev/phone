#include "chromecast/protocol.h"

#include <mbedtls/base64.h>
#include <mbedtls/ctr_drbg.h>
#include <mbedtls/entropy.h>
#include <mbedtls/pk.h>
#include <mbedtls/rsa.h>
#include <psa/crypto.h>

#include <algorithm>
#include <new>

#include "chromecast/json.h"
#include "common/owner.h"

namespace chromecast {
namespace {
struct Rsa {
  mbedtls_entropy_context entropy;
  mbedtls_ctr_drbg_context drbg;
  mbedtls_pk_context pk;
};

void destroy(Rsa* r) noexcept {
  mbedtls_pk_free(&r->pk);
  mbedtls_ctr_drbg_free(&r->drbg);
  mbedtls_entropy_free(&r->entropy);
  delete r;
}

auto failure(const char* message) { return std::unexpected(common::Error{message}); }

std::string text(const Json* value) { return as_string(value).value_or(""); }

std::string der_length(std::size_t size) {
  if (size < 0x80) return std::string(1, char(size));
  std::string bytes;
  for (; size; size >>= 8) bytes.insert(bytes.begin(), char(size & 0xff));
  return char(0x80 | bytes.size()) + bytes;
}

// SubjectPublicKeyInfo { rsaEncryption, BIT STRING { RSAPublicKey } }.
std::string wrap_pkcs1(const std::string& key) {
  static constexpr char kAlgorithm[] =
      "\x30\x0d\x06\x09\x2a\x86\x48\x86\xf7\x0d\x01\x01\x01\x05\x00";
  std::string bits = '\x03' + der_length(key.size() + 1) + '\0' + key;
  std::string inner = std::string(kAlgorithm, sizeof(kAlgorithm) - 1) + bits;
  return '\x30' + der_length(inner.size()) + inner;
}

char lower(char c) { return c >= 'A' && c <= 'Z' ? char(c - 'A' + 'a') : c; }

bool same_text(std::string_view a, std::string_view b) {
  return a.size() == b.size() && std::equal(a.begin(), a.end(), b.begin(),
                                            [](char x, char y) { return lower(x) == lower(y); });
}
}

const char* describe_state(int state) {
  switch (state) {
    case 10:
    case 11:
      return "Not connected";
    case 20:
      return "Scanning for Wi-Fi";
    case 21:
      return "Network not found";
    case 30:
      return "Joining Wi-Fi";
    case 31:
      return "Wrong password";
    case 32:
      return "Possibly wrong password";
    case 40:
      return "Getting an IP address";
    case 41:
      return "The router gave no IP address";
    case 50:
      return "Checking internet access";
    case 51:
    case 52:
      return "No internet through this router";
    case 53:
    case 54:
      return "The network needs a sign-in page";
    case 60:
      return "Connected";
    case 61:
      return "Connected, not saved yet";
    case 62:
      return "Wi-Fi saved, update pending";
    case 63:
    case 64:
      return "Connected, setup pending";
    default:
      return "Unknown state";
  }
}

bool failed_state(int state) {
  return state == 21 || state == 31 || state == 32 || state == 41 || state == 51 || state == 53 ||
         state == 54;
}

std::optional<DeviceInfo> parse_info(std::string_view json) {
  auto root = parse_json(json);
  if (!root || root->type != Json::Type::kObject) return std::nullopt;
  const Json* r = &*root;
  DeviceInfo d;
  d.name = text(find(r, {"name"}));
  d.model = text(find(r, {"device_info", "model_name"}));
  d.build = text(find(r, {"build_info", "cast_build_revision"}));
  if (d.build.empty()) d.build = text(find(r, {"cast_build_revision"}));
  d.mac = text(find(r, {"device_info", "mac_address"}));
  if (d.mac.empty()) d.mac = text(find(r, {"mac_address"}));
  d.udn = text(find(r, {"device_info", "ssdp_udn"}));
  if (d.udn.empty()) d.udn = text(find(r, {"ssdp_udn"}));
  d.hotspot_bssid = text(find(r, {"device_info", "hotspot_bssid"}));
  d.public_key = text(find(r, {"device_info", "public_key"}));
  if (d.public_key.empty()) d.public_key = text(find(r, {"public_key"}));
  d.ssid = text(find(r, {"wifi", "ssid"}));
  if (d.ssid.empty()) d.ssid = text(find(r, {"ssid"}));
  d.ip = text(find(r, {"net", "ip_address"}));
  if (d.ip.empty()) d.ip = text(find(r, {"ip_address"}));
  auto state = as_integer(find(r, {"setup", "setup_state"}));
  if (!state) state = as_integer(find(r, {"setup_state"}));
  d.state = state ? int(*state) : -1;
  d.signal = int(as_integer(find(r, {"wifi", "signal_level"})).value_or(0));
  d.keeps_hotspot =
      as_bool(find(r, {"device_info", "capabilities", "keep_hotspot_until_connected_supported"}))
          .value_or(false);
  // Anything else answering on the Cast ports is not a setup API.
  if (d.state < 0 && d.public_key.empty() && d.name.empty()) return std::nullopt;
  return d;
}

std::vector<WifiNetwork> parse_scan(std::string_view json) {
  std::vector<WifiNetwork> result;
  auto root = parse_json(json);
  if (!root) return result;
  const Json* list = &*root;
  if (list->type == Json::Type::kObject) {
    for (const char* key : {"networks", "scan_results", "results"})
      if (const Json* found = find(list, {key}); found && found->type == Json::Type::kArray) {
        list = found;
        break;
      }
  }
  if (list->type != Json::Type::kArray) return result;
  for (const Json& row : list->items) {
    auto ssid = as_string(find(&row, {"ssid"}));
    if (!ssid || ssid->empty()) continue;
    WifiNetwork n;
    n.ssid = *ssid;
    n.bssid = text(find(&row, {"bssid"}));
    n.signal = int(as_integer(find(&row, {"signal_level"})).value_or(-100));
    n.auth = int(as_integer(find(&row, {"wpa_auth"})).value_or(0));
    n.cipher = int(as_integer(find(&row, {"wpa_cipher"})).value_or(0));
    n.wpa_id = int(as_integer(find(&row, {"wpa_id"})).value_or(-1));
    n.frequency = int(as_integer(find(&row, {"frequency"})).value_or(0));
    if (const Json* aps = find(&row, {"ap_list"}); !n.frequency && aps && !aps->items.empty())
      n.frequency = int(as_integer(find(&aps->items[0], {"frequency"})).value_or(0));
    auto same = std::find_if(result.begin(), result.end(), [&](const WifiNetwork& o) {
      return o.ssid == n.ssid && o.auth == n.auth && o.cipher == n.cipher;
    });
    if (same == result.end()) result.push_back(n);
    else {
      if (n.wpa_id < 0) n.wpa_id = same->wpa_id;
      if (n.signal > same->signal) *same = n;
      else if (same->wpa_id < 0) same->wpa_id = n.wpa_id;
    }
  }
  std::stable_sort(result.begin(), result.end(),
                   [](const WifiNetwork& a, const WifiNetwork& b) { return a.signal > b.signal; });
  return result;
}

std::vector<std::string> parse_configured(std::string_view json) {
  std::vector<std::string> result;
  auto root = parse_json(json);
  if (!root) return result;
  const Json* list = &*root;
  if (list->type == Json::Type::kObject)
    for (const char* key : {"networks", "configured_networks"})
      if (const Json* found = find(list, {key}); found && found->type == Json::Type::kArray) {
        list = found;
        break;
      }
  if (list->type != Json::Type::kArray) return result;
  for (const Json& row : list->items)
    if (auto ssid = as_string(find(&row, {"ssid"}))) result.push_back(*ssid);
  return result;
}

// Open, WPA, WPA2, and WPA2/WPA3 transition networks with known ciphers, as
// the earlier terminal setup accepted. WEP, enterprise, and WPA3-only are not.
bool supported(const WifiNetwork& n) {
  bool auth = n.auth == 1 || n.auth == 5 || n.auth == 7 || n.auth == 9;
  bool cipher = n.cipher == 1 || n.cipher == 3 || n.cipher == 4 || n.cipher == 5;
  return auth && cipher;
}

bool needs_password(const WifiNetwork& n) { return n.auth != 1; }

bool valid_passphrase(std::string_view p) {
  if (p.size() == 64)
    return std::all_of(p.begin(), p.end(), [](char c) {
      return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
    });
  return p.size() >= 8 && p.size() <= 63 &&
         std::all_of(p.begin(), p.end(), [](char c) { return c >= 32 && c < 127; });
}

bool same_device(const DeviceInfo& a, const DeviceInfo& b) {
  if (!a.mac.empty() && !b.mac.empty()) return same_text(a.mac, b.mac);
  if (!a.udn.empty() && !b.udn.empty()) return same_text(a.udn, b.udn);
  return !a.public_key.empty() && a.public_key == b.public_key;
}

common::Result<std::string> encrypt_password(std::string_view public_key,
                                             std::string_view password) {
  std::string compact;
  for (char c : public_key)
    if (c != ' ' && c != '\n' && c != '\r' && c != '\t') compact += c;
  if (compact.empty()) return failure("The Chromecast did not send its public key");
  std::string der(compact.size(), '\0');
  std::size_t size = 0;
  if (mbedtls_base64_decode(reinterpret_cast<unsigned char*>(der.data()), der.size(), &size,
                            reinterpret_cast<const unsigned char*>(compact.data()),
                            compact.size()) != 0)
    return failure("The Chromecast's public key is not valid base64");
  der.resize(size);
  if (psa_crypto_init() != PSA_SUCCESS) return failure("Cannot initialize cryptography");
  common::Owner<Rsa> r(new (std::nothrow) Rsa);
  if (!r) return failure("Cannot allocate the encryption state");
  mbedtls_entropy_init(&r->entropy);
  mbedtls_ctr_drbg_init(&r->drbg);
  mbedtls_pk_init(&r->pk);
  // Accept SubjectPublicKeyInfo, then wrap the PKCS#1 RSAPublicKey that known
  // firmware sends in one, as the Google Home app does.
  auto parse = [&](const std::string& key) {
    mbedtls_pk_free(&r->pk);
    mbedtls_pk_init(&r->pk);
    return mbedtls_pk_parse_public_key(&r->pk, reinterpret_cast<const unsigned char*>(key.data()),
                                       key.size()) == 0 &&
           mbedtls_pk_get_type(&r->pk) == MBEDTLS_PK_RSA;
  };
  if (!parse(der) && !parse(wrap_pkcs1(der)))
    return failure("The Chromecast's public key is not an RSA key");
  mbedtls_rsa_context* rsa = mbedtls_pk_rsa(r->pk);
  std::size_t length = mbedtls_rsa_get_len(rsa);
  if (length < 128 || password.size() + 11 > length)
    return failure("The Chromecast's public key cannot encrypt this password");
  static constexpr unsigned char kPersonal[] = "chromecast-password";
  if (mbedtls_ctr_drbg_seed(&r->drbg, mbedtls_entropy_func, &r->entropy, kPersonal,
                            sizeof(kPersonal)) != 0)
    return failure("Cannot seed the random generator");
  std::string cipher(length, '\0');
  if (mbedtls_rsa_pkcs1_encrypt(rsa, mbedtls_ctr_drbg_random, &r->drbg, password.size(),
                                reinterpret_cast<const unsigned char*>(password.data()),
                                reinterpret_cast<unsigned char*>(cipher.data())) != 0)
    return failure("Cannot encrypt the password");
  std::string encoded(4 * ((length + 2) / 3) + 1, '\0');
  if (mbedtls_base64_encode(reinterpret_cast<unsigned char*>(encoded.data()), encoded.size(), &size,
                            reinterpret_cast<const unsigned char*>(cipher.data()),
                            cipher.size()) != 0)
    return failure("Cannot encode the encrypted password");
  encoded.resize(size);
  return encoded;
}

std::string connect_body(const WifiNetwork& n, std::string_view encrypted, bool keep_hotspot) {
  std::string body = "{\"ssid\":" + quote(n.ssid) + ",\"wpa_auth\":" + std::to_string(n.auth) +
                     ",\"wpa_cipher\":" + std::to_string(n.cipher) +
                     ",\"wpa_id\":" + std::to_string(std::max(n.wpa_id, 0)) +
                     ",\"scan_ssid\":0,\"enc_passwd\":" + quote(encrypted);
  // Devices advertising keep_hotspot_until_connected_supported keep their setup
  // hotspot up while joining, so the result can be read there.
  if (keep_hotspot) body += ",\"keep_hotspot_until_connected\":true";
  return body + "}";
}
}
