#include <arpa/inet.h>
#include <ifaddrs.h>
#include <netinet/in.h>

#include <cstdio>

#include "chromecast/discovery.h"
#include "chromecast/http.h"
#include "chromecast/protocol.h"

using namespace chromecast;

// Without an address, list Cast devices on wlan0 by mDNS and by port sweep.
int discover() {
  ifaddrs* list = nullptr;
  if (getifaddrs(&list) != 0) return 1;
  for (ifaddrs* i = list; i; i = i->ifa_next) {
    if (!i->ifa_addr || i->ifa_addr->sa_family != AF_INET || std::string(i->ifa_name) != "wlan0")
      continue;
    std::uint32_t local = ntohl(reinterpret_cast<sockaddr_in*>(i->ifa_addr)->sin_addr.s_addr);
    std::uint32_t mask = ntohl(reinterpret_cast<sockaddr_in*>(i->ifa_netmask)->sin_addr.s_addr);
    int prefix = __builtin_popcount(mask);
    std::printf("wlan0 %s/%d\n", format_address(local).c_str(), prefix);
    for (auto a : query_mdns(0, local, kMdnsAddress, kMdnsPort, 1500))
      std::printf("  mDNS: %s\n", format_address(a).c_str());
    for (auto a : sweep_subnet(0, local, prefix, 8008, 800))
      std::printf("  port 8008: %s\n", format_address(a).c_str());
  }
  freeifaddrs(list);
  return 0;
}

int main(int argc, char** argv) {
  if (argc < 2) return discover();
  in_addr parsed;
  if (inet_pton(AF_INET, argv[1], &parsed) != 1) return 2;
  for (bool tls : {true, false}) {
    Endpoint e{0, ntohl(parsed.s_addr), std::uint16_t(tls ? 8443 : 8008), tls};
    auto response = request(e, "GET", kInfoPath);
    if (!response) {
      std::printf("%s: %s\n", tls ? "https" : "http", response.error().message.c_str());
      continue;
    }
    auto info = parse_info(response->body);
    std::printf("%s: HTTP %d, %zu bytes, parsed %d\n", tls ? "https" : "http", response->status,
                response->body.size(), bool(info));
    if (!info) continue;
    std::printf("  %s (%s %s) state %d %s on %s at %s, mac %s hotspot %s keep %d\n",
                info->name.c_str(), info->model.c_str(), info->build.c_str(), info->state,
                describe_state(info->state), info->ssid.c_str(), info->ip.c_str(),
                info->mac.c_str(), info->hotspot_bssid.c_str(), info->keeps_hotspot);
    auto encrypted = encrypt_password(info->public_key, "probe-password");
    std::printf("  encrypt: %s\n", encrypted ? "ok" : encrypted.error().message.c_str());
    auto scan = request(e, "GET", "/setup/scan_results");
    if (scan)
      for (auto& n : parse_scan(scan->body))
        std::printf("  %-24s %4d dBm auth %d cipher %d id %d %d MHz supported %d\n", n.ssid.c_str(),
                    n.signal, n.auth, n.cipher, n.wpa_id, n.frequency, supported(n));
    auto configured = request(e, "GET", "/setup/configured_networks");
    if (configured)
      for (auto& s : parse_configured(configured->body)) std::printf("  saved: %s\n", s.c_str());
  }
}
