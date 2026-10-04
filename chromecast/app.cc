#include "chromecast/app.h"

#include <algorithm>
#include <cmath>
#include <new>
#include <utility>

#include "common/gpu/renderer.h"

namespace chromecast {
using common::Error;
using common::Owner;
using common::Result;

namespace {
constexpr gpu::Color kBackground{0, 0, 0, 0};
// Shift tapped twice within this time locks capitals.
constexpr double kDoubleTap = .35;
// Holding delete repeats it after a delay.
constexpr double kRepeatDelay = .5, kRepeatInterval = .06;
constexpr std::size_t kMaximumPassword = 64;
}

struct App {
  Owner<Session> session;
  Owner<gpu::Renderer> renderer;
  Snapshot snapshot;
  // Snapshots of commands before this one are stale.
  std::uint64_t awaiting = 0;
  // Password entry for the chosen network.
  bool entering = false;
  int chosen = -1;
  std::string password, password_ssid;
  bool reveal = false;
  Keyboard keyboard;
  double shift_time = -10;
  // List scrolling in dp, and the screen it belongs to.
  float scroll = 0;
  Screen scrolled = Screen::kDevices;
  gpu::Rect content;
  float density = 1, touch_slop = 8;
  Layout layout;
  bool laid_out = false, dirty = true, moved = false, dragging = false;
  Hit pressed;
  float down_x = 0, down_y = 0, scroll_start = 0;
  double repeat_at = -1;
  bool paste_requested = false;
  bool wallpaper_requested = false;
  std::vector<unsigned char> wallpaper;
  int wallpaper_width = 0, wallpaper_height = 0;
  int blink = -1;
  float fps = 0;
  double fps_since = 0, fps_last = 0;
  unsigned fps_frames = 0;
};

namespace {
void wipe(std::string& text) {
  std::fill(text.begin(), text.end(), '\0');
  text.clear();
}

Screen screen(const App& a) { return screen_for(a.snapshot, a.entering); }

ViewInput view_input(const App& a) {
  ViewInput in;
  in.screen = screen(a);
  in.snapshot = &a.snapshot;
  in.chosen = a.chosen;
  in.password = a.password;
  in.reveal = a.reveal;
  in.keyboard = a.keyboard;
  in.scroll = a.scroll;
  in.has_wallpaper = !a.wallpaper.empty();
  in.fps = a.fps;
  return in;
}

void record_frame(App& a, double now) {
  if (!a.fps_last || now <= a.fps_last || now - a.fps_last > 1) {
    a.fps = 0;
    a.fps_since = now;
    a.fps_frames = 0;
  } else {
    ++a.fps_frames;
    if (double elapsed = now - a.fps_since; elapsed >= .5) {
      a.fps = a.fps_frames / elapsed;
      a.fps_since = now;
      a.fps_frames = 0;
    }
  }
  a.fps_last = now;
}

// Commands update the snapshot at once, so the next frame shows the new
// screen before the session reports progress.
void go_search(App& a) {
  a.entering = false;
  a.snapshot.stage = Stage::kSearching;
  a.snapshot.target.clear();
  a.snapshot.message.clear();
  a.awaiting = search(*a.session);
}

void go_open(App& a, int index) {
  a.entering = false;
  if (index >= 0 && std::size_t(index) < a.snapshot.devices.size())
    a.snapshot.device = a.snapshot.devices[index];
  else a.snapshot.device = {};
  a.snapshot.stage = a.snapshot.device.reachable ? Stage::kScanning : Stage::kHotspot;
  a.snapshot.networks.clear();
  a.awaiting = open_device(*a.session, index);
}

void choose(App& a, int index) {
  if (index < 0 || std::size_t(index) >= a.snapshot.networks.size()) return;
  const WifiNetwork& n = a.snapshot.networks[index];
  if (!supported(n)) return;
  a.chosen = index;
  if (n.ssid != a.password_ssid) wipe(a.password);
  a.password_ssid = n.ssid;
  a.keyboard = {};
  a.reveal = false;
  a.entering = true;
}

void connect(App& a) {
  if (!a.entering || a.chosen < 0 || std::size_t(a.chosen) >= a.snapshot.networks.size()) return;
  const WifiNetwork& n = a.snapshot.networks[a.chosen];
  if (needs_password(n) && !valid_passphrase(a.password)) return;
  a.snapshot.target = n.ssid;
  a.snapshot.stage = Stage::kSending;
  a.snapshot.message.clear();
  a.entering = false;
  a.awaiting = join(*a.session, a.chosen, needs_password(n) ? a.password : std::string());
}

void press_key(App& a, int code, double now) {
  Keyboard& k = a.keyboard;
  switch (code) {
    case kShiftKey:
      if (k.shift == 0) k.shift = 1;
      else if (k.shift == 1) k.shift = now - a.shift_time < kDoubleTap ? 2 : 0;
      else k.shift = 0;
      a.shift_time = now;
      break;
    case kDeleteKey:
      if (!a.password.empty()) a.password.pop_back();
      break;
    case kSymbolsKey:
      k.page = 1;
      break;
    case kMoreKey:
      k.page = 2;
      break;
    case kLettersKey:
      k.page = 0;
      break;
    default:
      if (char c = key_char(code, k); c && a.password.size() < kMaximumPassword) {
        a.password += c;
        if (k.page == 0 && k.shift == 1) k.shift = 0;
      }
  }
}

void activate(App& a, Hit hit) {
  switch (hit.target) {
    case Target::kBack:
      back(a);
      break;
    case Target::kDevice:
      go_open(a, hit.index);
      break;
    case Target::kHotspot:
      go_open(a, -1);
      break;
    case Target::kSearch:
      go_search(a);
      break;
    case Target::kNetwork:
      choose(a, hit.index);
      break;
    case Target::kRescan:
    case Target::kRetry:
      a.entering = false;
      a.snapshot.stage = Stage::kScanning;
      a.snapshot.networks.clear();
      a.snapshot.target.clear();
      a.awaiting = rescan(*a.session);
      break;
    case Target::kReveal:
      a.reveal = !a.reveal;
      break;
    case Target::kPaste:
      a.paste_requested = true;
      break;
    case Target::kWallpaper:
      a.wallpaper_requested = true;
      break;
    case Target::kConnect:
      connect(a);
      break;
    case Target::kDone:
      go_search(a);
      break;
    default:
      break;
  }
}

bool same(Hit a, Hit b) { return a.target == b.target && a.index == b.index; }

float scroll_limit(const App& a) {
  if (!a.laid_out || a.layout.scale <= 0) return 0;
  return std::max(0.f, a.layout.list_height - a.layout.list.h / a.layout.scale);
}

void settle_screen(App& a) {
  Screen now = screen(a);
  if (now != a.scrolled) {
    a.scrolled = now;
    a.scroll = 0;
    a.pressed = {};
  }
}
}

Result<Owner<App>> create_app(Platform& platform, const SessionConfig& config) {
  Owner<App> app(new (std::nothrow) App);
  if (!app) return std::unexpected(Error{"Cannot allocate the application"});
  auto session = create_session(platform, config);
  if (!session) return std::unexpected(session.error());
  app->session = std::move(*session);
  app->snapshot = take_snapshot(*app->session);
  app->awaiting = search(*app->session);
  return app;
}

void destroy(App* app) noexcept {
  app->renderer.reset();
  // Joins the worker, which releases Wi-Fi requests.
  app->session.reset();
  wipe(app->password);
  delete app;
}

Result<void> attach_window(App& a, ANativeWindow* window, int width, int height) {
  a.renderer.reset();
  a.laid_out = false;
  auto renderer = gpu::create_overlay_renderer(window, width, height, true);
  if (!renderer) return std::unexpected(renderer.error());
  if (!a.wallpaper.empty()) {
    auto uploaded =
        gpu::set_overlay_image(**renderer, a.wallpaper_width, a.wallpaper_height, a.wallpaper);
    if (!uploaded) return uploaded;
  }
  a.renderer = std::move(*renderer);
  a.dirty = true;
  return {};
}

void detach_window(App& a) {
  a.renderer.reset();
  a.laid_out = false;
  a.pressed = {};
  a.repeat_at = -1;
}

gpu::Renderer* get_renderer(App& a) { return a.renderer.get(); }

void set_content(App& a, gpu::Rect content) {
  if (content.x == a.content.x && content.y == a.content.y && content.w == a.content.w &&
      content.h == a.content.h)
    return;
  a.content = content;
  a.pressed = {};
  a.dirty = true;
}

void set_density(App& a, float pixels_per_dp) {
  if (!std::isfinite(pixels_per_dp) || pixels_per_dp <= 0 || pixels_per_dp == a.density) return;
  a.density = pixels_per_dp;
  a.pressed = {};
  a.dirty = true;
}

void set_touch_slop(App& a, float pixels) {
  if (std::isfinite(pixels) && pixels > 0) a.touch_slop = pixels;
}

void touch(App& a, Touch action, float x, float y, double now) {
  if (!a.laid_out) return;
  Hit hit = hit_test(a.layout, x, y);
  float dx = x - a.down_x, dy = y - a.down_y;
  if (action != Touch::kDown && dx * dx + dy * dy > a.touch_slop * a.touch_slop) a.moved = true;
  switch (action) {
    case Touch::kDown:
      a.down_x = x;
      a.down_y = y;
      a.moved = a.dragging = false;
      a.scroll_start = a.scroll;
      a.pressed = hit;
      a.repeat_at = -1;
      // Keys type on touch down, like a keyboard; delete repeats while held.
      if (hit.target == Target::kKey) {
        press_key(a, hit.index, now);
        if (hit.index == kDeleteKey) a.repeat_at = now + kRepeatDelay;
      }
      break;
    case Touch::kMove:
      if (a.moved && a.pressed.target != Target::kKey &&
          gpu::contains(a.layout.list, a.down_x, a.down_y))
        a.dragging = true;
      if (a.dragging) {
        a.scroll = std::clamp(a.scroll_start - dy / a.layout.scale, 0.f, scroll_limit(a));
        a.pressed = {};
      } else if (a.moved || !same(hit, a.pressed)) {
        if (a.pressed.target != Target::kKey) a.pressed = {};
      }
      break;
    case Touch::kUp:
      if (a.pressed.target != Target::kNone && a.pressed.target != Target::kKey &&
          same(hit, a.pressed) && !a.moved)
        activate(a, hit);
      a.pressed = {};
      a.repeat_at = -1;
      break;
    case Touch::kCancel:
      a.pressed = {};
      a.repeat_at = -1;
      break;
  }
  settle_screen(a);
  a.dirty = true;
}

bool back(App& a) {
  a.pressed = {};
  a.dirty = true;
  switch (screen(a)) {
    case Screen::kDevices:
      return false;
    case Screen::kPassword:
      a.entering = false;
      break;
    default:
      go_search(a);
  }
  settle_screen(a);
  return true;
}

void enter(App& a) {
  if (screen(a) != Screen::kPassword) return;
  connect(a);
  settle_screen(a);
  a.dirty = true;
}

bool take_paste_request(App& a) {
  bool requested = a.paste_requested;
  a.paste_requested = false;
  return requested;
}

bool take_wallpaper_request(App& a) { return std::exchange(a.wallpaper_requested, false); }

Result<void> set_wallpaper(App& a, int width, int height, std::vector<unsigned char> rgba) {
  if (!rgba.empty() && (width <= 0 || height <= 0 || width > 2048 || height > 2048 ||
                        rgba.size() != std::size_t(width) * height * 4))
    return std::unexpected(Error{"The wallpaper image has invalid dimensions"});
  if (a.renderer) {
    auto uploaded = gpu::set_overlay_image(*a.renderer, width, height, rgba);
    if (!uploaded) return uploaded;
  }
  a.wallpaper = std::move(rgba);
  a.wallpaper_width = width;
  a.wallpaper_height = height;
  a.dirty = true;
  return {};
}

void paste(App& a, const std::string& text) {
  if (screen(a) != Screen::kPassword) return;
  // Passwords copied from elsewhere often carry a trailing newline.
  for (char c : text)
    if (c >= 32 && c < 127 && a.password.size() < kMaximumPassword) a.password += c;
  a.dirty = true;
}

int app_fd(const App& a) { return session_fd(*a.session); }

void on_session(App& a) {
  Snapshot next = take_snapshot(*a.session);
  if (next.command < a.awaiting) return;
  // A password screen stays open while the device list or scan refreshes.
  if (next.stage != Stage::kNetworks) a.entering = false;
  else if (a.entering && (a.chosen < 0 || std::size_t(a.chosen) >= next.networks.size() ||
                          next.networks[a.chosen].ssid != a.password_ssid))
    a.entering = false;
  a.snapshot = std::move(next);
  if (a.snapshot.stage == Stage::kDone) {
    wipe(a.password);
    a.password_ssid.clear();
  }
  settle_screen(a);
  a.dirty = true;
}

bool update(App& a, double now) {
  // Show zero once rendering stops, without continuously redrawing an idle app.
  if (a.fps > 0 && now - a.fps_last > 1) {
    a.fps = 0;
    a.dirty = true;
  }
  if (a.repeat_at >= 0 && now >= a.repeat_at && a.pressed.target == Target::kKey &&
      a.pressed.index == kDeleteKey) {
    press_key(a, kDeleteKey, now);
    a.repeat_at = now + kRepeatInterval;
    a.dirty = true;
  }
  bool animating = !a.wallpaper.empty();
  for (const Widget& w : a.layout.widgets)
    if (w.kind == Kind::kSpinner || (w.kind == Kind::kStep && w.status == 1)) animating = true;
  if (screen(a) == Screen::kPassword) {
    int blink = int(now / .5) % 2;
    if (blink != a.blink) a.dirty = true;
    a.blink = blink;
  }
  if (animating || a.repeat_at >= 0) a.dirty = true;
  return a.dirty && a.renderer;
}

Result<bool> draw(App& a, double now) {
  if (!a.renderer) return false;
  auto& r = *a.renderer;
  auto prepared = gpu::prepare_frame(r, false);
  if (!prepared || !*prepared) return prepared;
  auto stats = gpu::get_stats(r);
  settle_screen(a);
  ViewInput in = view_input(a);
  Layout layout = layout_view(stats.width, stats.height, a.content, a.density, in);
  // Keep the list inside its range after rotations and refreshes.
  float limit = std::max(0.f, layout.list_height - layout.list.h / std::max(layout.scale, 1e-3f));
  if (a.scroll > limit) {
    a.scroll = limit;
    in.scroll = limit;
    layout = layout_view(stats.width, stats.height, a.content, a.density, in);
  }
  auto begun = gpu::render_overlay(r, kBackground, now);
  if (!begun || !*begun) return begun;
  draw_view(r, layout, a.pressed, now);
  auto presented = gpu::present(r);
  if (presented && *presented) {
    // Count completed submissions at their intended presentation times.
    record_frame(a, now);
    a.layout = std::move(layout);
    a.laid_out = true;
    a.dirty = false;
  }
  return presented;
}

Result<void> redraw(App& a, double now) {
  for (int attempt = 0; attempt < 4; ++attempt) {
    auto drawn = draw(a, now);
    if (!drawn) return std::unexpected(drawn.error());
    if (*drawn) return gpu::wait_frame(*a.renderer);
    if (!a.renderer) return {};
  }
  return {};
}

Result<bool> check_surface(App& a) {
  if (!a.renderer) return false;
  auto changed = gpu::surface_changed(*a.renderer);
  if (!changed) return std::unexpected(changed.error());
  if (*changed) {
    a.pressed = {};
    a.dirty = true;
  }
  return *changed;
}

Screen get_screen(const App& a) { return screen(a); }

const Snapshot& get_snapshot(const App& a) { return a.snapshot; }

const std::string& get_password(const App& a) { return a.password; }

const Layout& get_layout(const App& a) { return a.layout; }
}
