#pragma once

#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "common/result.h"

// The Chromecast /setup/* API, as used by the Google Home app's setup flow.
namespace chromecast {
inline constexpr char kInfoPath[] =
    "/setup/eureka_info?params=version,name,build_info,device_info,net,wifi,setup";
inline constexpr char kBasicInfoPath[] = "/setup/eureka_info";

// setup_state values.
inline constexpr int kWrongPassword = 31, kConnected = 60, kNotSaved = 61, kSaved = 62;

struct DeviceInfo {
  std::string name, model, build, mac, udn, hotspot_bssid;
  // Base64 RSA public key, PKCS#1 RSAPublicKey DER on known firmware.
  std::string public_key;
  // The Wi-Fi network the device is on and its address there, when connected.
  std::string ssid, ip;
  int state = -1, signal = 0;
  bool keeps_hotspot = false;
};

struct WifiNetwork {
  std::string ssid, bssid;
  int signal = -100, auth = 0, cipher = 0, frequency = 0;
  // The device's id for a network it already has saved, or -1.
  int wpa_id = -1;
};

// A short description of a setup_state for people.
const char* describe_state(int state);
// A setup_state that means joining the requested network failed.
bool failed_state(int state);
std::optional<DeviceInfo> parse_info(std::string_view json);
// Entries with the same SSID and security merge into the strongest one;
// results are sorted by signal.
std::vector<WifiNetwork> parse_scan(std::string_view json);
std::vector<std::string> parse_configured(std::string_view json);
bool supported(const WifiNetwork& network);
bool needs_password(const WifiNetwork& network);
// WPA passphrases are 8-63 printable ASCII characters or 64 hexadecimal digits.
bool valid_passphrase(std::string_view passphrase);
// Same hardware: MAC address, then SSDP UDN, then public key.
bool same_device(const DeviceInfo& a, const DeviceInfo& b);
// Base64 RSA PKCS#1 v1.5 ciphertext of the password under the device's key.
common::Result<std::string> encrypt_password(std::string_view public_key,
                                             std::string_view password);
std::string connect_body(const WifiNetwork& network, std::string_view encrypted, bool keep_hotspot);
}
