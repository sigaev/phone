# Chromecast Wi-Fi

A native C++23 Android app that moves a Chromecast to a different Wi-Fi
network, whether or not the Chromecast can still reach its current one. It
talks to the Chromecast's local `/setup/*` API, the one the Google Home app
uses during setup, so it needs no Google account, cloud service, or
Bluetooth. It is based on the terminal tool in `/root/chromecast`, which
set up this phone's Chromecast (`Chromecast6745`, an `anchovy` device on
firmware 1.36) on 2026-09-24. Like the other apps, it uses Android's built-in
`NativeActivity`, so the APK has no Java sources or DEX code.

## Using it

The first screen lists the Chromecasts on every Wi-Fi network the phone has,
plus the ones the app has seen before:

- **On a network** (green): the Chromecast is reachable, so the app works over
  that network.
- **Out of reach** (gray): a Chromecast seen before that did not answer. If it
  cannot reach its Wi-Fi, it starts its open setup hotspot after a while and the
  TV shows its setup screen. Tapping it asks Android to join that hotspot by the
  BSSID the Chromecast reported earlier. If 45 seconds pass without a
  connection, the app tries again with the Chromecast's name as an SSID prefix.
- **Setup hotspot** (bottom button): for a Chromecast the app has never seen,
  such as one reset to factory settings. The app asks Android to join a hotspot
  whose SSID starts with `Chromecast`.

Joining a hotspot uses Android's Wi-Fi network request, so Android shows a
prompt to connect to the device. The connection is only for this app, and
the phone stays on its normal Wi-Fi when the hardware supports two
connections, as the Pixel 8 Pro does.

Opening a Chromecast asks it to scan for Wi-Fi and lists what it found, with
signal, security, and whether it is the current or a saved network. Networks
whose security the setup API cannot handle, such as enterprise and WPA3-only
networks, are greyed out. Choosing a network opens the password screen. It has
an on-screen keyboard with letters, digits, and every printable ASCII symbol,
**Show**/**Hide**, and **Paste** from the clipboard. **Connect** is enabled
once the password is a valid WPA passphrase: 8 to 63 characters, or 64
hexadecimal digits. Open networks skip the password.

If contact is lost during a scan, or the device refuses it or returns invalid
results, the app reports that failure and offers a retry. It only reports
finding no networks after successfully reading an empty scan.

The progress screen then shows four steps:

1. **Send the network.** The app reads the Chromecast's RSA public key from
   `eureka_info`, encrypts the password with RSA PKCS#1 v1.5, and posts
   `connect_wifi`. The password never leaves the app in plain text. The
   Chromecast may drop the connection while it switches networks; the app
   continues checking, without treating that disconnect as a successful join.
2. **Join.** The app polls the Chromecast for up to 40 seconds and stops on a
   wrong password or another join failure. If the Chromecast reports
   `keep_hotspot_until_connected_supported`, the app asks it to keep its
   hotspot up while it joins, so the result can be read there.
3. **Find it on the new network.** The app looks on all of the phone's Wi-Fi
   networks, identifying the Chromecast by MAC address. If the phone is not on
   the new network, Android is asked after 15 seconds to join it for this app,
   using the password just entered. Android may show another prompt. If the
   Chromecast comes back on its old network, the join failed, and the app says
   so. The **Join** checkmark stays off until the device confirms its new
   network. If Android refuses the phone's network request, the app reports
   the error and explains how to connect through Settings. Discovery stops after
   roughly three minutes when no device is found.
4. **Save.** Once the Chromecast reports it is on the new network, the app
   calls `save_wifi` until `configured_networks` lists that network.

A failure says what went wrong and, where a different password might help,
offers **Try again**, which rescans and keeps the typed password for
correction. **Done** and Back return to the device list and release every
Wi-Fi network the app requested. The password is cleared after success.

## How it talks to Chromecasts

- **HTTPS on port 8443** without certificate checks, because Chromecasts use
  self-signed certificates. The app falls back to HTTP on port 8008 only when
  TLS fails after a TCP connection. On this firmware, both ports accept the
  setup `POST`s on the home network without a local authorization token.
- `connect_wifi` must be sent with `Content-Type: application/json` exactly;
  devices reject a charset parameter.
- **Discovery** sends a legacy-unicast mDNS query for `_googlecast._tcp.local`.
  Replies come back unicast, so no multicast lock is needed. It also checks
  which hosts of the phone's /24 subnet accept connections on port 8008, then
  reads `eureka_info` from each candidate. On a hotspot, the Chromecast is at
  192.168.255.249.
- Sockets are bound to the chosen Android network with
  `android_setsocknetwork`. Wi-Fi networks, including the ones the app
  requested, come from `ConnectivityManager` through JNI.
- Network requests use `ConnectivityManager.requestNetwork` with a
  `WifiNetworkSpecifier` and a retained framework `NetworkCallback`, released
  with `unregisterNetworkCallback` when the flow ends. The concrete framework
  callback keeps the request alive while the worker polls `getAllNetworks()`
  with a deadline, so the app needs no Java callback class. A `PendingIntent`
  request would be released by Android shortly after its broadcast, dropping
  the connection before setup finishes.
- Devices seen before are kept in `devices.json` in the app's private storage:
  name, model, MAC address, UDN, hotspot BSSID, and last network and address.
  No passwords or keys are stored.

TLS and RSA come from [Mbed TLS](https://github.com/Mbed-TLS/mbedtls) 3.6.7,
fetched as a Bazel Central Registry module. Everything else is in this
directory: a JSON parser, the HTTP client, mDNS, and the setup flows.

## Code

- `json`, `http`, `protocol`, and `discovery` implement the setup API,
  parsing, password encryption, and LAN discovery.
- `session` runs the flows on a background thread and publishes snapshots
  through an eventfd. Every command replaces the one running, and snapshots of
  older commands are ignored. `platform.h` declares the phone's Wi-Fi
  operations. `android_platform` implements them with JNI.
- `view` lays out, hit-tests, and draws the screens with `//common/gpu`'s
  overlay renderer. `app` handles navigation, scrolling, and the keyboard.
  `main.cc` is the `NativeActivity` glue, adapted from Sudoku's.
- `fakes` is a test-only simulated Chromecast and phone. The Chromecast is an
  HTTPS and HTTP server on loopback addresses that stand for its home-network
  address and its hotspot. It has a real RSA key, decrypts the passwords it
  receives, and switches, fails, or saves networks as a device does. The fake
  `Platform` joins networks when a request names one the device can be found
  on.

## Build and test

After the shared [toolchain setup](../README.md#toolchain-setup), run from the
workspace root:

```sh
bazel build //chromecast
bazel test //chromecast:protocol_test //chromecast:session_test //chromecast:app_test
```

Output: `bazel-bin/chromecast/chromecast.apk`, about 260 KB. The application
ID is `dev.demo.chromecast`. The minimum and target Android API is 36. Android
grants all of its permissions at install:

- `INTERNET` and `ACCESS_NETWORK_STATE`
- `CHANGE_NETWORK_STATE`, `ACCESS_WIFI_STATE`, and `CHANGE_WIFI_STATE`

The tests cover the following:

- `protocol_test`: JSON, device info shaped like the real device's,
  merging and sorting scan results, passphrase rules, RSA encryption with
  PKCS#1 and SPKI keys (decrypted with the private key), the `connect_wifi`
  body, mDNS replies with and without address records, and the subnet sweep.
- `session_test`: runs every flow against the fakes:
  - moving on the home network to a network the phone is not on;
  - a wrong password that sends the device back to its old network;
  - setup through a hotspot that stays up;
  - setup through a hotspot that drops;
  - reaching a remembered device's hotspot by BSSID;
  - refused or unanswered phone network requests, discovery timeouts, and
    cancellation that releases an outstanding request without confirming a join.
  - empty scans, rejected scans, malformed results, and a hotspot connection
    lost between starting a scan and reading its results.
- `app_test`: checks the layouts of every screen at five window sizes for
  overlap, safe areas, and hit targets. It then taps through the real app on the
  GPU with offscreen rendering, typing passwords with symbols on the on-screen
  keyboard. Pass a directory to save PPM screenshots:

```sh
mkdir -p /data/data/com.termux/files/usr/tmp/chromecast-shots
bazel run //chromecast:app_test -- /data/data/com.termux/files/usr/tmp/chromecast-shots
```

`--config=vulkan_validation` runs `app_test` with Khronos validation.

`//chromecast:probe` is a read-only check of real devices. With no argument it
lists Cast devices on `wlan0`. With an IPv4 address it prints the device's
info, last scan, and saved networks:

```sh
bazel run //chromecast:probe -- 192.168.1.80
```

## Install on this phone

Copy the APK to Termux's home and open Android's package installer, as for
[Native Buttons](../native_buttons/README.md#install-on-this-phone):

```sh
mkdir -p /data/data/com.termux/files/home/chromecast
cp bazel-bin/chromecast/chromecast.apk /data/data/com.termux/files/home/chromecast/chromecast.apk
chmod 0400 /data/data/com.termux/files/usr/libexec/termux-am/am.apk
am start --user 0 -W -a android.intent.action.INSTALL_PACKAGE \
    -d content://com.termux.files/data/data/com.termux/files/home/chromecast/chromecast.apk \
    -t application/vnd.android.package-archive -f 0x10000001 \
    -p com.google.android.packageinstaller
```

Termux needs the same temporary `allow-external-apps = true` setting in
`/data/data/com.termux/files/home/.termux/termux.properties` as for Native
Buttons.

## Limits

- Only Chromecasts with the HTTPS setup API are supported. Google TV models,
  which are set up over Bluetooth, are not.
- The hotspot of a renamed Chromecast that the app has never seen may not match
  the `Chromecast` prefix. Open the app once while the device is reachable, so
  it remembers the hotspot BSSID.
- If a Chromecast never starts its hotspot, holding its button resets it to
  factory settings, after which **Setup hotspot** works. The app never resets,
  reboots, or renames devices.
- The fakes test the flows, but the hotspot and network requests have only run
  in tests, not yet against a real Chromecast or Android's prompt.
