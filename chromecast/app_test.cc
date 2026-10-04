#if defined(NATIVE_BUTTONS_VULKAN_VALIDATION)
#include <dlfcn.h>
#endif
#include <poll.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <time.h>

#include <cstdio>
#include <string>
#include <vector>

#include "chromecast/app.h"
#include "chromecast/fakes.h"
#include "common/gpu/renderer.h"

#if defined(NATIVE_BUTTONS_VULKAN_VALIDATION)
namespace gpu {
unsigned validation_error_count();
}
#endif

namespace {
using namespace chromecast;
using gpu::Rect;

#if defined(NATIVE_BUTTONS_VULKAN_VALIDATION)
// The CLI has no Activity to configure Android's layer search path. This
// test-only bootstrap uses the platform GraphicsEnv, as //native_buttons:gpu_probe does.
bool prepare_validation() {
  const char* path = std::getenv("NATIVE_BUTTONS_VULKAN_LAYER_PATH");
  using GetEnvironment = void* (*)();
  using SetPaths = void (*)(void*, void*, const std::string&);
  auto get_environment = reinterpret_cast<GetEnvironment>(
      dlsym(RTLD_DEFAULT, "_ZN7android11GraphicsEnv11getInstanceEv"));
  auto set_paths = reinterpret_cast<SetPaths>(
      dlsym(RTLD_DEFAULT,
            "_ZN7android11GraphicsEnv13setLayerPathsEPNS_21NativeLoaderNamespaceERKNSt3__"
            "112basic_stringIcNS3_11char_traitsIcEENS3_9allocatorIcEEEE"));
  if (!path || !get_environment || !set_paths) return false;
  set_paths(get_environment(), nullptr, std::string(path));
  return true;
}
#endif

int failures = 0;

void check(bool condition, const char* message) {
  if (!condition) {
    std::fprintf(stderr, "FAILED: %s\n", message);
    ++failures;
  }
}

bool overlaps(Rect a, Rect b) {
  constexpr float kEpsilon = .01f;
  return a.x + kEpsilon < b.x + b.w && b.x + kEpsilon < a.x + a.w && a.y + kEpsilon < b.y + b.h &&
         b.y + kEpsilon < a.y + a.h;
}

bool within(Rect inner, Rect outer) {
  return inner.x >= outer.x - .5f && inner.y >= outer.y - .5f &&
         inner.x + inner.w <= outer.x + outer.w + .5f &&
         inner.y + inner.h <= outer.y + outer.h + .5f;
}

float center_x(Rect r) { return r.x + r.w * .5f; }

float center_y(Rect r) { return r.y + r.h * .5f; }

double monotonic() {
  timespec now;
  clock_gettime(CLOCK_MONOTONIC, &now);
  return double(now.tv_sec) + now.tv_nsec * 1e-9;
}

Snapshot sample(Stage stage) {
  Snapshot s;
  s.stage = stage;
  Device home;
  home.info.name = "Living Room";
  home.info.ssid = "Home";
  home.info.ip = "192.168.1.80";
  home.reachable = true;
  Device away;
  away.info.name = "Bedroom";
  Device setup;
  setup.info.name = "Chromecast6745";
  setup.reachable = setup.hotspot = true;
  for (int i = 0; i < 4; ++i) s.devices.insert(s.devices.end(), {home, away, setup});
  s.device = home;
  for (int i = 0; i < 14; ++i) {
    WifiNetwork n;
    n.ssid = i == 0 ? "Home" : "Network with a rather long name " + std::to_string(i);
    n.signal = -40 - 4 * i;
    n.auth = i % 5 == 4 ? 8 : i % 3 == 2 ? 1 : 7;
    n.cipher = n.auth == 1 ? 1 : 4;
    n.wpa_id = i == 0 ? 0 : -1;
    s.networks.push_back(n);
  }
  s.target = "Network with a rather long name 1";
  s.message =
      "Bedroom could not join Network with a rather long name 1 and went back to Home. "
      "Check the password and that the network is in range.";
  s.retry = true;
  s.failed = Stage::kFinding;
  s.prompt = true;
  s.joined = stage == Stage::kSaving || stage == Stage::kDone;
  return s;
}

void check_screen(const char* name, int width, int height, Rect content, float density,
                  ViewInput in) {
  Layout l = layout_view(width, height, content, density, in);
  check(!l.widgets.empty(), "screens have widgets");
  std::vector<const Widget*> fixed;
  for (const Widget& w : l.widgets) {
    if (w.target == Target::kNone) continue;
    check(w.bounds.w > 0 && w.bounds.h > 0, "targets have area");
    if (w.scrolls) {
      check(w.bounds.x >= l.safe.x - .5f && w.bounds.x + w.bounds.w <= l.safe.x + l.safe.w + .5f,
            "list rows fit the safe width");
      continue;
    }
    if (!within(w.bounds, l.safe)) {
      std::fprintf(stderr, "%s: target %d outside the safe area\n", name, int(w.target));
      check(false, "controls stay inside the safe area");
    }
    fixed.push_back(&w);
  }
  for (std::size_t i = 0; i < fixed.size(); ++i)
    for (std::size_t j = i + 1; j < fixed.size(); ++j)
      if (overlaps(fixed[i]->bounds, fixed[j]->bounds)) {
        std::fprintf(stderr, "%s: targets %d and %d overlap\n", name, int(fixed[i]->target),
                     int(fixed[j]->target));
        check(false, "controls do not overlap");
      }
  for (const Widget& w : l.widgets) {
    if (w.target == Target::kNone || !w.enabled) continue;
    float x = center_x(w.bounds), y = center_y(w.bounds);
    if (w.scrolls && !gpu::contains(l.list, x, y)) continue;
    Hit hit = hit_test(l, x, y);
    check(hit.target == w.target && hit.index == w.index, "controls are hit where they are drawn");
    if (w.kind == Kind::kKey || w.kind == Kind::kButton || w.kind == Kind::kPrimary)
      check(w.bounds.h >= 30 * density * .6f, "controls stay large enough to tap");
  }
  if (l.list.h > 0) {
    for (const Widget& w : l.widgets)
      if (!w.scrolls && w.target != Target::kNone && w.target != Target::kBack &&
          w.kind != Kind::kSpinner)
        check(!overlaps(w.bounds, l.list), "fixed controls stay off the list");
  }
}

void check_layouts() {
  struct Size {
    const char* name;
    int width, height;
    Rect content;
    float density;
  } sizes[] = {
      {"phone", 1344, 2992, {0, 145, 1344, 2992 - 145 - 72}, 3},
      {"landscape", 2992, 1344, {145, 72, 2992 - 145 - 72, 1344 - 72}, 3},
      {"split screen", 1344, 1380, {0, 0, 1344, 1380}, 3},
      {"tablet", 2560, 1600, {0, 72, 2560, 1600 - 72 - 96}, 2},
      {"small window", 520, 420, {}, 1.5f},
  };

  Stage stages[] = {Stage::kDevices, Stage::kSearching, Stage::kHotspot, Stage::kNetworks,
                    Stage::kJoining, Stage::kFinding,   Stage::kDone,    Stage::kFailed};
  for (const Size& size : sizes)
    for (Stage stage : stages)
      for (bool password : {false, true}) {
        if (password && stage != Stage::kNetworks) continue;
        Snapshot s = sample(stage);
        ViewInput in;
        in.snapshot = &s;
        in.screen = screen_for(s, password);
        in.chosen = 1;
        in.password = "hunter22";
        check_screen(size.name, size.width, size.height, size.content, size.density, in);
        in.keyboard.page = 2;
        in.chosen = 2;
        if (password)
          check_screen(size.name, size.width, size.height, size.content, size.density, in);
        s.target.clear();
        in.screen = screen_for(s, password);
        check_screen(size.name, size.width, size.height, size.content, size.density, in);
      }
  Snapshot s = sample(Stage::kNetworks);
  check(screen_for(s, true) == Screen::kPassword && screen_for(s, false) == Screen::kNetworks,
        "the password screen is chosen locally");
  s.stage = Stage::kFailed;
  check(screen_for(s, false) == Screen::kProgress, "join failures show their progress");
  s.target.clear();
  check(screen_for(s, false) == Screen::kResult, "other failures show a result");
  check(printable("Caf\xc3\xa9 \xf0\x9f\x98\x80!") == "Caf? ?!", "unprintable text is replaced");
  Keyboard shifted{0, 1};
  check(key_char('q', shifted) == 'Q' && key_char('q', {}) == 'q' && key_char('1', shifted) == '1',
        "shift capitalizes letters");
  for (Stage stage : {Stage::kFinding, Stage::kFailed, Stage::kSaving, Stage::kDone}) {
    Snapshot progress = sample(stage);
    ViewInput in;
    in.snapshot = &progress;
    in.screen = Screen::kProgress;
    auto layout = layout_view(1080, 2400, {}, 3, in);
    int step = 0;
    for (const Widget& widget : layout.widgets) {
      if (widget.kind != Kind::kStep || step++ != 1) continue;
      check(progress.joined ? widget.status == 2 : widget.status == 0,
            "Join gets a green check only after the Chromecast confirms the target network");
      check(progress.joined || widget.detail == "Connection not yet confirmed",
            "discovery and discovery failure keep the join explicitly unconfirmed");
    }
    check(step == 4, "the progress screen has four steps");
  }
}

std::string output_directory;
constexpr int kAnyIndex = -1000;

void capture(App& app, const char* name) {
  if (output_directory.empty()) return;
  std::string path = output_directory + "/" + name + ".ppm";
  if (auto result = gpu::capture_frame(*get_renderer(app), path.c_str()); !result)
    check(false, "screenshot is written");
}

bool render(App& app) {
  auto result = redraw(app, monotonic());
  if (!result) std::fprintf(stderr, "%s\n", result.error().message.c_str());
  return bool(result);
}

template <typename Done>
bool pump(App& app, Done done, double seconds = 30) {
  double deadline = monotonic() + seconds;
  while (monotonic() < deadline) {
    pollfd waiting{app_fd(app), POLLIN, 0};
    if (poll(&waiting, 1, 50) > 0) on_session(app);
    update(app, monotonic());
    if (!render(app)) return false;
    if (done()) return true;
  }
  return false;
}

const Widget* find_widget(App& app, Target target, int index = kAnyIndex,
                          const char* label = nullptr) {
  for (const Widget& w : get_layout(app).widgets)
    if (w.target == target && (index == kAnyIndex || w.index == index) &&
        (!label || w.label == label))
      return &w;
  return nullptr;
}

bool tap(App& app, const Widget* w) {
  if (!w) return false;
  float x = center_x(w->bounds), y = center_y(w->bounds);
  double now = monotonic();
  touch(app, Touch::kDown, x, y, now);
  touch(app, Touch::kUp, x, y, now);
  return render(app);
}

// Type text on the on-screen keyboard, switching pages and shift as a person would.
bool type(App& app, const std::string& text) {
  for (char c : text) {
    bool letter = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z');
    int code = letter ? (c | 0x20) : c;
    for (int attempt = 0; attempt < 4; ++attempt) {
      const Widget* key = find_widget(app, Target::kKey, code);
      if (key && letter) {
        bool upper = c < 'a';
        const Widget* shift = find_widget(app, Target::kKey, kShiftKey);
        if (shift && (shift->status != 0) != upper) {
          if (!tap(app, shift)) return false;
          continue;
        }
      }
      if (key) {
        if (!tap(app, key)) return false;
        break;
      }
      const Widget* page = find_widget(app, Target::kKey, letter ? kLettersKey : kSymbolsKey);
      if (!page && !letter) page = find_widget(app, Target::kKey, kMoreKey);
      if (!page || !tap(app, page)) return false;
    }
  }
  return get_password(app).ends_with(text);
}

struct World {
  common::Owner<FakeDevice> device;
  common::Owner<Platform> platform;
  common::Owner<App> app;
};

World make_world(FakeDeviceConfig c, const std::vector<std::string>& phone,
                 const std::string& directory) {
  World w;
  auto device = create_fake_device(c);
  if (!device) return w;
  w.device = std::move(*device);
  w.platform = create_fake_platform(*w.device, phone);
  SessionConfig config;
  config.directory = directory;
  config.https_port = https_port(*w.device);
  config.http_port = http_port(*w.device);
  config.mdns_group = 0x7f000001;
  config.mdns_port = 9;
  config.hotspot_address = c.hotspot;
  config.time_scale = .05;
  auto app = create_app(*w.platform, config);
  if (!app) return w;
  w.app = std::move(*app);
  App& a = *w.app;
  if (auto result = attach_window(a, nullptr, 1344, 2992); !result) {
    std::fprintf(stderr, "%s\n", result.error().message.c_str());
    w.app.reset();
    return w;
  }
  set_density(a, 3);
  set_touch_slop(a, 24);
  set_content(a, {0, 145, 1344, 2992 - 145 - 72});
  return w;
}

FakeDeviceConfig home_device() {
  FakeDeviceConfig c;
  c.name = "Living Room";
  c.networks = {{"Home", "home-password"}, {"Upstairs", "Up-#2 {ok}~"}, {"Cafe", "", 1}};
  return c;
}

const Widget* network_row(App& app, const char* ssid) {
  return find_widget(app, Target::kNetwork, kAnyIndex, ssid);
}

void test_move(const std::string& directory) {
  World w = make_world(home_device(), {"Home"}, directory);
  check(bool(w.app), "the app starts");
  if (!w.app) return;
  App& a = *w.app;
  check(pump(a, [&] { return find_widget(a, Target::kDevice, 0) != nullptr; }),
        "the Chromecast is listed");
  capture(a, "devices");
  check(tap(a, find_widget(a, Target::kDevice, 0)), "the device opens");
  check(pump(a, [&] { return network_row(a, "Upstairs") != nullptr; }), "its networks are listed");
  capture(a, "networks");
  check(tap(a, network_row(a, "Upstairs")) && get_screen(a) == Screen::kPassword,
        "choosing a network asks for its password");
  const Widget* connect = find_widget(a, Target::kConnect);
  check(connect && !connect->enabled, "Connect waits for a valid password");
  check(type(a, "Up-#2 {ok}~x"), "the keyboard types letters, capitals, and symbols");
  check(tap(a, find_widget(a, Target::kKey, kDeleteKey)) && get_password(a) == "Up-#2 {ok}~",
        "delete removes the last character");
  capture(a, "password-hidden");
  check(tap(a, find_widget(a, Target::kReveal)), "the password can be shown");
  capture(a, "password");
  connect = find_widget(a, Target::kConnect);
  check(connect && connect->enabled && tap(a, connect), "Connect starts the move");
  check(get_screen(a) == Screen::kProgress, "progress shows at once");
  capture(a, "progress");
  check(pump(a, [&] { return get_snapshot(a).stage == Stage::kDone; }), "the move completes");
  capture(a, "done");
  check(
      get_record(*w.device).last_password == "Up-#2 {ok}~" && current_ssid(*w.device) == "Upstairs",
      "the device received the typed password");
  check(get_password(a).empty(), "the password is cleared after success");
  check(tap(a, find_widget(a, Target::kDone)) && get_screen(a) == Screen::kDevices,
        "Done returns to the device list");
  check(pump(a, [&] { return get_snapshot(a).stage == Stage::kDevices; }), "the list refreshes");
}

void test_failure(const std::string& directory) {
  World w = make_world(home_device(), {"Home", "Upstairs"}, directory);
  if (!w.app) return;
  App& a = *w.app;
  check(pump(a, [&] { return find_widget(a, Target::kDevice, 0) != nullptr; }), "listed again");
  tap(a, find_widget(a, Target::kDevice, 0));
  pump(a, [&] { return network_row(a, "Upstairs") != nullptr; });
  tap(a, network_row(a, "Upstairs"));
  check(type(a, "wrong-pass"), "a wrong password is typed");
  enter(a);
  check(pump(
            a, [&] { return get_snapshot(a).stage == Stage::kFailed; }, 40),
        "a wrong password fails");
  capture(a, "failed");
  check(find_widget(a, Target::kRetry) != nullptr, "the failure offers another try");
  check(tap(a, find_widget(a, Target::kRetry)), "Try again rescans");
  check(pump(a, [&] { return network_row(a, "Upstairs") != nullptr; }), "networks return");
  tap(a, network_row(a, "Upstairs"));
  check(get_password(a) == "wrong-pass", "the password is kept to correct it");
  check(back(a) && get_screen(a) == Screen::kNetworks, "back leaves the password screen");
  check(back(a) && get_screen(a) == Screen::kDevices && !back(a),
        "back returns to the first screen");
}

void test_hotspot(const std::string& directory) {
  FakeDeviceConfig c = home_device();
  // Never seen before, so it is found by its factory hotspot name.
  c.name = "Chromecast6745";
  c.setup_mode = true;
  World w = make_world(c, {"Home"}, directory);
  if (!w.app) return;
  App& a = *w.app;
  check(pump(a, [&] { return get_snapshot(a).stage == Stage::kDevices; }), "searching ends");
  capture(a, "empty");
  check(tap(a, find_widget(a, Target::kHotspot)), "Setup hotspot starts");
  capture(a, "hotspot");
  check(pump(a, [&] { return network_row(a, "Cafe") != nullptr; }),
        "the hotspot is joined and scanned");
  capture(a, "hotspot-networks");
  check(tap(a, network_row(a, "Cafe")) && get_screen(a) == Screen::kPassword &&
            !find_widget(a, Target::kKey),
        "open networks need no password");
  capture(a, "open-network");
  check(tap(a, find_widget(a, Target::kConnect)), "Connect joins the open network");
  check(pump(a, [&] { return get_snapshot(a).stage == Stage::kDone; }), "setup completes");
  check(current_ssid(*w.device) == "Cafe" && get_record(*w.device).last_password.empty(),
        "the device joins the open network");
}
}

int main(int argc, char** argv) {
#if defined(NATIVE_BUTTONS_VULKAN_VALIDATION)
  if (!prepare_validation()) {
    std::fprintf(stderr, "Use //tools:vulkan_validation_runner\n");
    return 1;
  }
#endif
  if (argc > 1) output_directory = argv[1];
  check_layouts();
  const char* tmp = getenv("TEST_TMPDIR");
  if (!tmp) tmp = getenv("TMPDIR");
  std::string root =
      std::string(tmp ? tmp : "/data/data/com.termux/files/usr/tmp") + "/chromecast-app.XXXXXX";
  if (!mkdtemp(root.data())) return 1;
  for (const char* name : {"/move", "/failure", "/hotspot"}) mkdir((root + name).c_str(), 0700);
  test_move(root + "/move");
  test_failure(root + "/failure");
  test_hotspot(root + "/hotspot");
  std::string command = "rm -rf " + root;
  (void)!system(command.c_str());
#if defined(NATIVE_BUTTONS_VULKAN_VALIDATION)
  check(gpu::validation_error_count() == 0, "Vulkan validation reports no errors");
#endif
  if (failures) return 1;
  std::printf("app_test passed\n");
  return 0;
}
