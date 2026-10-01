#include "chromecast/view.h"

#include <algorithm>
#include <cmath>

#include "common/gpu/renderer.h"

namespace chromecast {
using gpu::Color;
using gpu::Rect;
using gpu::Renderer;

namespace {
constexpr Color kWhite{1, 1, 1};
constexpr Color kInk{.122f, .122f, .122f};
constexpr Color kGray{.373f, .388f, .408f};
constexpr Color kFaint{.741f, .757f, .776f};
constexpr Color kBlue{.102f, .451f, .910f};
constexpr Color kBluePressed{.082f, .341f, .690f};
constexpr Color kCard{.945f, .953f, .957f};
constexpr Color kCardPressed{.871f, .886f, .902f};
constexpr Color kLine{.855f, .863f, .878f};
constexpr Color kGreen{.094f, .502f, .220f};
constexpr Color kRed{.851f, .188f, .145f};
constexpr Color kKey{.910f, .918f, .929f};
constexpr Color kKeySpecial{.808f, .820f, .843f};
constexpr Color kKeyPressed{.690f, .710f, .741f};
constexpr Color kSuccessPanel{.902f, .957f, .918f};
constexpr Color kErrorPanel{.992f, .918f, .910f};

constexpr float kListGap = 10, kBottomBar = 84;

float center_x(Rect r) { return r.x + r.w * .5f; }

float center_y(Rect r) { return r.y + r.h * .5f; }

// Rectangles in dp within a centered column, converted to window pixels.
struct Builder {
  Layout& l;
  float ox, oy, s;
  // Column left edge and width, and the content height, in dp.
  float x, width, height;

  Rect at(float left, float top, float w, float h) const {
    return {ox + left * s, oy + top * s, w * s, h * s};
  }

  Widget& add(Kind kind, float left, float top, float w, float h, std::string label = {},
              Target target = Target::kNone, int index = -1) {
    Widget widget;
    widget.kind = kind;
    widget.target = target;
    widget.index = index;
    widget.bounds = at(left, top, w, h);
    widget.label = std::move(label);
    l.widgets.push_back(std::move(widget));
    return l.widgets.back();
  }

  Widget& row(Kind kind, float top, float h, std::string label = {}, Target target = Target::kNone,
              int index = -1) {
    return add(kind, x, top, width, h, std::move(label), target, index);
  }

  void header(const std::string& title) {
    add(Kind::kLink, x - 12, 8, 48, 48, {}, Target::kBack);
    add(Kind::kTitle, x + 40, 8, width - 40, 48, title);
  }

  // One or two buttons along the bottom edge.
  void bottom(Kind first, std::string first_label, Target first_target, bool first_enabled,
              Kind second = Kind::kText, std::string second_label = {},
              Target second_target = Target::kNone) {
    float top = height - 16 - 52;
    if (second_target == Target::kNone) {
      row(first, top, 52, std::move(first_label), first_target).enabled = first_enabled;
      return;
    }
    float half = (width - 12) * .5f;
    add(first, x, top, half, 52, std::move(first_label), first_target).enabled = first_enabled;
    add(second, x + half + 12, top, half, 52, std::move(second_label), second_target);
  }

  // The scrolling list between top and the bottom bar.
  float list(float top, float scroll) {
    float bottom = height - kBottomBar;
    l.list = at(0, top, l.safe.w / s, std::max(0.f, bottom - top));
    return top - scroll;
  }
};

std::string device_detail(const Device& d) {
  if (d.hotspot) return "Setup hotspot - ready to set up";
  if (d.reachable)
    return d.info.ssid.empty() ? "On this phone's Wi-Fi at " + d.info.ip
                               : "On " + printable(d.info.ssid) + " - " + d.info.ip;
  return "Out of reach - tap to use its setup hotspot";
}

int bars(int signal) { return signal >= -55 ? 4 : signal >= -65 ? 3 : signal >= -75 ? 2 : 1; }

std::string device_name(const Snapshot& s) {
  return s.device.info.name.empty() ? "Chromecast" : printable(s.device.info.name);
}

void layout_devices(Builder& b, const ViewInput& in) {
  const Snapshot& s = *in.snapshot;
  bool searching = s.stage == Stage::kSearching;
  b.row(Kind::kTitle, 12, 48, "Chromecast Wi-Fi");
  if (searching) b.add(Kind::kSpinner, b.x + b.width - 32, 20, 32, 32);
  b.row(Kind::kSubtitle, 58, 24,
        searching           ? "Looking on this phone's Wi-Fi..."
        : s.devices.empty() ? "No Chromecast found"
                            : "Choose a Chromecast to move to another network");
  float y = b.list(96, in.scroll);
  for (std::size_t i = 0; i < s.devices.size(); ++i) {
    const Device& d = s.devices[i];
    Widget& w =
        b.row(Kind::kRow, y, 76, d.info.name.empty() ? "Chromecast" : printable(d.info.name),
              Target::kDevice, int(i));
    w.detail = device_detail(d);
    w.status = d.hotspot ? 1 : d.reachable ? 0 : 2;
    w.scrolls = true;
    y += 76 + kListGap;
  }
  b.l.list_height = s.devices.empty() ? 0 : s.devices.size() * (76 + kListGap) - kListGap;
  if (s.devices.empty() && !searching)
    b.row(Kind::kText, 104, 120,
          "No Chromecast answered on this phone's Wi-Fi. If yours cannot reach its network, "
          "tap Setup hotspot.");
  b.bottom(Kind::kButton, "Search again", Target::kSearch, !searching, Kind::kButton,
           "Setup hotspot", Target::kHotspot);
}

void layout_working(Builder& b, const ViewInput& in) {
  const Snapshot& s = *in.snapshot;
  b.header(device_name(s));
  b.add(Kind::kSpinner, b.x + b.width * .5f - 28, 120, 56, 56);
  bool hotspot = s.stage == Stage::kHotspot;
  Widget& w = b.row(Kind::kText, 204, 32,
                    hotspot ? "Joining the Chromecast's setup hotspot"
                            : device_name(s) + " is scanning for Wi-Fi networks");
  w.status = 2;
  if (hotspot)
    b.row(Kind::kText, 244, 120,
          "Approve Android's prompt to connect to the Chromecast. Its hotspot is on when the "
          "TV shows the setup screen.")
        .status = 1;
}

void layout_networks(Builder& b, const ViewInput& in) {
  const Snapshot& s = *in.snapshot;
  b.header(device_name(s));
  b.row(Kind::kSubtitle, 60, 24,
        s.device.hotspot             ? "Reached through its setup hotspot"
        : s.device.info.ssid.empty() ? "Choose its new Wi-Fi network"
                                     : "Now on " + printable(s.device.info.ssid));
  float y = b.list(96, in.scroll);
  for (std::size_t i = 0; i < s.networks.size(); ++i) {
    const WifiNetwork& n = s.networks[i];
    Widget& w = b.row(Kind::kRow, y, 64, printable(n.ssid), Target::kNetwork, int(i));
    bool current = !s.device.hotspot && n.ssid == s.device.info.ssid;
    w.detail = !supported(n)       ? "Unsupported security"
               : current           ? "Current network"
               : n.wpa_id >= 0     ? "Saved on the Chromecast"
               : needs_password(n) ? "Secured"
                                   : "Open";
    if (n.frequency >= 4900) w.detail += " - 5 GHz";
    w.status = bars(n.signal);
    w.locked = needs_password(n);
    w.enabled = supported(n);
    w.scrolls = true;
    y += 64 + 8;
  }
  b.l.list_height = s.networks.empty() ? 0 : s.networks.size() * 72.f - 8;
  if (!s.message.empty()) b.row(Kind::kText, 104, 80, s.message);
  b.bottom(Kind::kButton, "Scan again", Target::kRescan, true);
}

struct KeySpec {
  int code;
  float units;
};

void layout_keyboard(Builder& b, float top, float key, const Keyboard& k) {
  constexpr float kGap = 6;
  std::vector<KeySpec> rows[4];
  auto letters = [](std::vector<KeySpec>& row, const char* text) {
    for (const char* c = text; *c; ++c) row.push_back({*c, 1});
  };
  if (k.page == 0) {
    letters(rows[0], "qwertyuiop");
    letters(rows[1], "asdfghjkl");
    rows[2].push_back({kShiftKey, 1.5f});
    letters(rows[2], "zxcvbnm");
    rows[3] = {{kSymbolsKey, 1.5f}, {',', 1}, {' ', 5}, {'.', 1}, {'@', 1.5f}};
  } else {
    letters(rows[0], k.page == 1 ? "1234567890" : "~`|^={}[]\\");
    letters(rows[1], k.page == 1 ? "@#$_&-+()/" : "<>%_&+-");
    rows[2].push_back({k.page == 1 ? kMoreKey : kSymbolsKey, 1.5f});
    letters(rows[2], "*\"':;!?");
    rows[3] = {{kLettersKey, 1.5f}, {',', 1}, {' ', 5}, {'.', 1}, {k.page == 1 ? '=' : '@', 1.5f}};
  }
  rows[2].push_back({kDeleteKey, 1.5f});
  float unit = (b.width - 9 * kGap) / 10;
  for (int r = 0; r < 4; ++r) {
    float total = -kGap;
    for (const KeySpec& spec : rows[r]) total += spec.units * unit + (spec.units - 1) * kGap + kGap;
    float x = b.x + (b.width - total) * .5f, y = top + r * (key + 8);
    for (const KeySpec& spec : rows[r]) {
      float w = spec.units * unit + (spec.units - 1) * kGap;
      Widget& widget = b.add(Kind::kKey, x, y, w, key, {}, Target::kKey, spec.code);
      if (spec.code > 0) widget.label = std::string(1, key_char(spec.code, k));
      if (spec.code == ' ') widget.label = "space";
      if (spec.code == kSymbolsKey) widget.label = "?123";
      if (spec.code == kMoreKey) widget.label = "=\\<";
      if (spec.code == kLettersKey) widget.label = "ABC";
      if (spec.code == kShiftKey) widget.status = k.shift;
      x += w + kGap;
    }
  }
}

void layout_password(Builder& b, const ViewInput& in) {
  const Snapshot& s = *in.snapshot;
  if (in.chosen < 0 || std::size_t(in.chosen) >= s.networks.size()) return;
  const WifiNetwork& n = s.networks[in.chosen];
  b.header("Join " + printable(n.ssid));
  if (!needs_password(n)) {
    b.row(Kind::kText, 80, 60, printable(n.ssid) + " is an open network. No password is needed.");
    b.row(Kind::kPrimary, 150, 52, "Connect", Target::kConnect);
    return;
  }
  b.row(Kind::kSubtitle, 60, 24, "Password");
  Widget& field = b.row(Kind::kField, 88, 52, in.password);
  field.locked = !in.reveal;
  b.add(Kind::kLink, b.x, 144, 96, 40, in.reveal ? "Hide" : "Show", Target::kReveal);
  b.add(Kind::kLink, b.x + b.width - 96, 144, 96, 40, "Paste", Target::kPaste);
  bool valid = valid_passphrase(in.password);
  Widget& hint =
      b.row(Kind::kSubtitle, 186, 20,
            in.password.size() > 63 ? "Too long: 63 characters at most" : "8 to 63 characters");
  hint.status = !valid && (in.password.size() > 63 || in.password.size() >= 8) ? 1 : 0;
  b.row(Kind::kPrimary, 214, 52, "Connect", Target::kConnect).enabled = valid;
  float key = 48;
  layout_keyboard(b, b.height - 12 - 4 * key - 3 * 8, key, in.keyboard);
}

int step_of(Stage stage) {
  switch (stage) {
    case Stage::kSending:
      return 0;
    case Stage::kJoining:
      return 1;
    case Stage::kFinding:
      return 2;
    case Stage::kSaving:
      return 3;
    case Stage::kDone:
      return 4;
    default:
      return 0;
  }
}

void layout_progress(Builder& b, const ViewInput& in) {
  const Snapshot& s = *in.snapshot;
  std::string target = printable(s.target);
  b.header("Moving " + device_name(s));
  b.row(Kind::kSubtitle, 60, 24, "to " + target);
  bool failed = s.stage == Stage::kFailed;
  int current = step_of(failed ? s.failed : s.stage);
  const std::string labels[4] = {"Send the network to the Chromecast", "Join " + target,
                                 "Find it on " + target, "Save the network"};
  for (int i = 0; i < 4; ++i) {
    Widget& w = b.row(Kind::kStep, 96 + i * 60, 52, labels[i]);
    w.status = i < current ? 2 : i > current ? 0 : failed ? 3 : 1;
    if (i == current && !failed) {
      if (i == 1) w.detail = "This can take up to a minute";
      if (i == 2)
        w.detail = s.prompt ? "Approve Android's prompt to join " + target
                            : "Searching this phone's Wi-Fi networks";
    }
  }
  if (s.stage == Stage::kDone) {
    b.row(Kind::kSuccess, 344, 96, s.message);
    b.bottom(Kind::kPrimary, "Done", Target::kDone, true);
  } else if (failed) {
    b.row(Kind::kError, 344, 112, s.message);
    if (s.retry)
      b.bottom(Kind::kButton, "Back", Target::kDone, true, Kind::kPrimary, "Try again",
               Target::kRetry);
    else b.bottom(Kind::kPrimary, "Back", Target::kDone, true);
  }
}

void layout_result(Builder& b, const ViewInput& in) {
  const Snapshot& s = *in.snapshot;
  b.header(device_name(s));
  b.row(Kind::kError, 80, 140, s.message);
  b.bottom(Kind::kButton, "Back", Target::kDone, true, Kind::kPrimary, "Try again", Target::kRetry);
}

float minimum_height(Screen screen) {
  switch (screen) {
    case Screen::kPassword:
      return 520;
    case Screen::kProgress:
      return 540;
    case Screen::kWorking:
      return 380;
    default:
      return 400;
  }
}

struct Painter {
  Renderer& r;
  float s;
  // Capital height per unit of text height.
  float cap;

  float px(float dp) const { return dp * s; }

  float width(const std::string& value, float cap_height) const {
    return gpu::measure_text(r, value.c_str(), cap_height / cap).w;
  }

  // Center capitals vertically on cy; align -1 left, 0 center, 1 right.
  void text(const std::string& value, float x, float cy, float cap_height, Color color,
            int align = -1) const {
    float height = cap_height / cap, baseline = cy + cap_height * .5f;
    if (align > 0) x -= width(value, cap_height);
    gpu::draw_text(r, value.c_str(), x, baseline, height, color, align == 0);
  }

  std::string fit(const std::string& value, float limit, float cap_height) const {
    if (width(value, cap_height) <= limit) return value;
    std::string shorter = value;
    while (!shorter.empty() && width(shorter + "...", cap_height) > limit) shorter.pop_back();
    return shorter + "...";
  }

  // Word-wrapped text from the top of bounds.
  void wrapped(const std::string& value, Rect bounds, float cap_height, Color color) const {
    float line = cap_height * 2, y = bounds.y + cap_height;
    std::string current;
    std::size_t at = 0;
    while (at <= value.size()) {
      std::size_t end = value.find(' ', at);
      if (end == std::string::npos) end = value.size();
      std::string word = value.substr(at, end - at);
      std::string candidate = current.empty() ? word : current + " " + word;
      if (!current.empty() && width(candidate, cap_height) > bounds.w) {
        text(current, bounds.x, y, cap_height, color);
        y += line;
        current = word;
      } else {
        current = candidate;
      }
      at = end + 1;
    }
    if (!current.empty()) text(fit(current, bounds.w, cap_height), bounds.x, y, cap_height, color);
  }

  void dot(float x, float y, float radius, Color color) const {
    gpu::draw_rect(r, {x - radius, y - radius, 2 * radius, 2 * radius}, radius, color);
  }

  void line(float x0, float y0, float x1, float y1, float thickness, Color color) const {
    gpu::draw_line(r, x0, y0, x1, y1, thickness, color);
    dot(x0, y0, thickness * .5f, color);
    dot(x1, y1, thickness * .5f, color);
  }

  void arc(float cx, float cy, float radius, float from, float to, float thickness,
           Color color) const {
    int segments = std::max(4, int(std::fabs(to - from) / (gpu::kPi / 16)));
    float x = cx + std::cos(from) * radius, y = cy + std::sin(from) * radius;
    for (int i = 1; i <= segments; ++i) {
      float angle = from + (to - from) * i / segments;
      float nx = cx + std::cos(angle) * radius, ny = cy + std::sin(angle) * radius;
      line(x, y, nx, ny, thickness, color);
      x = nx;
      y = ny;
    }
  }

  void spinner(Rect b, double time, Color color) const {
    float radius = std::min(b.w, b.h) * .4f;
    float start = float(std::fmod(time * 5.5, 2 * gpu::kPi));
    float sweep = gpu::kPi * (1.f + .5f * std::sin(float(std::fmod(time * 2.2, 2 * gpu::kPi))));
    arc(center_x(b), center_y(b), radius, start, start + sweep, radius * .22f, color);
  }

  void check(float x, float y, float i, Color color) const {
    line(x - i * .3f, y, x - i * .08f, y + i * .24f, i * .16f, color);
    line(x - i * .08f, y + i * .24f, x + i * .34f, y - i * .26f, i * .16f, color);
  }

  void cross(float x, float y, float i, Color color) const {
    line(x - i * .25f, y - i * .25f, x + i * .25f, y + i * .25f, i * .16f, color);
    line(x - i * .25f, y + i * .25f, x + i * .25f, y - i * .25f, i * .16f, color);
  }

  void lock(float x, float y, float i, Color color) const {
    gpu::draw_rect(r, {x - i * .32f, y - i * .05f, i * .64f, i * .5f}, i * .08f, color);
    arc(x, y - i * .08f, i * .2f, gpu::kPi, 2 * gpu::kPi, i * .1f, color);
    line(x - i * .2f, y - i * .08f, x - i * .2f, y, i * .1f, color);
    line(x + i * .2f, y - i * .08f, x + i * .2f, y, i * .1f, color);
  }

  void signal(float x, float y, float i, int bars, Color on, Color off) const {
    float w = i * .17f, gap = i * .08f;
    for (int k = 0; k < 4; ++k) {
      float h = i * (.25f + .25f * k);
      Rect bar{x + k * (w + gap) - i * .5f, y + i * .5f - h, w, h};
      gpu::draw_rect(r, bar, w * .3f, k < bars ? on : off);
    }
  }

  // Points left, or right when forward.
  void chevron(float x, float y, float i, Color color, bool forward = false) const {
    float d = forward ? -i : i;
    line(x + d * .15f, y - i * .3f, x - d * .15f, y, i * .1f, color);
    line(x - d * .15f, y, x + d * .15f, y + i * .3f, i * .1f, color);
  }

  void shift(float x, float y, float i, int state, Color color) const {
    gpu::draw_triangle(r, x, y - i * .32f, x - i * .3f, y, x + i * .3f, y, color);
    gpu::draw_rect(r, {x - i * .13f, y - i * .01f, i * .26f, i * .26f}, 0, color);
    if (state == 2) gpu::draw_rect(r, {x - i * .3f, y + i * .34f, i * .6f, i * .08f}, 0, color);
  }

  void backspace(float x, float y, float i, Color color) const {
    float left = x - i * .42f, right = x + i * .38f, top = y - i * .26f, bottom = y + i * .26f;
    float tip = left + i * .22f;
    line(left, y, tip, top, i * .08f, color);
    line(tip, top, right, top, i * .08f, color);
    line(right, top, right, bottom, i * .08f, color);
    line(right, bottom, tip, bottom, i * .08f, color);
    line(tip, bottom, left, y, i * .08f, color);
    cross(x + i * .04f, y, i * .55f, color);
  }
};

bool is(Hit a, const Widget& w) { return a.target == w.target && a.index == w.index; }

void draw_widget(const Painter& p, const Widget& w, Hit pressed, double time) {
  Rect b = w.bounds;
  bool down = w.target != Target::kNone && w.enabled && is(pressed, w);
  switch (w.kind) {
    case Kind::kTitle:
      p.text(p.fit(w.label, b.w, p.px(17)), b.x, center_y(b), p.px(17), kInk);
      break;
    case Kind::kSubtitle:
      p.text(p.fit(w.label, b.w, p.px(11)), b.x, center_y(b), p.px(11), w.status ? kRed : kGray);
      break;
    case Kind::kText:
      p.wrapped(w.label, b, p.px(w.status == 2 ? 13 : 11.5f), w.status == 1 ? kGray : kInk);
      break;
    case Kind::kError:
    case Kind::kSuccess: {
      bool good = w.kind == Kind::kSuccess;
      gpu::draw_rect(p.r, b, p.px(14), good ? kSuccessPanel : kErrorPanel);
      float icon = p.px(22), cx = b.x + p.px(28), cy = b.y + p.px(28);
      p.dot(cx, cy, icon * .5f, good ? kGreen : kRed);
      if (good) p.check(cx, cy, icon * .9f, kWhite);
      else p.cross(cx, cy, icon * .9f, kWhite);
      Rect text{b.x + p.px(52), b.y + p.px(16), b.w - p.px(68), b.h - p.px(24)};
      p.wrapped(w.label, text, p.px(11.5f), kInk);
      break;
    }
    case Kind::kButton:
    case Kind::kPrimary: {
      bool primary = w.kind == Kind::kPrimary;
      Color fill = primary ? (w.enabled ? (down ? kBluePressed : kBlue) : kLine)
                           : (down ? kCardPressed : kCard);
      gpu::draw_rect(p.r, b, b.h * .5f, fill);
      Color ink = primary ? kWhite : w.enabled ? kBlue : kFaint;
      p.text(p.fit(w.label, b.w - p.px(16), p.px(12)), center_x(b), center_y(b), p.px(12), ink, 0);
      break;
    }
    case Kind::kLink:
      if (down) gpu::draw_rect(p.r, b, std::min(b.w, b.h) * .5f, kCard);
      if (w.target == Target::kBack) p.chevron(center_x(b), center_y(b), p.px(26), kInk);
      else p.text(w.label, center_x(b), center_y(b), p.px(12), kBlue, 0);
      break;
    case Kind::kRow: {
      gpu::draw_rect(p.r, b, p.px(14), down ? kCardPressed : kCard);
      Color ink = w.enabled ? kInk : kFaint;
      float left = b.x + p.px(18), right = b.x + b.w - p.px(18);
      if (w.target == Target::kDevice) {
        Color status = w.status == 0 ? kGreen : w.status == 1 ? kBlue : kFaint;
        p.dot(left + p.px(6), center_y(b), p.px(6), status);
        left += p.px(24);
        p.chevron(right - p.px(4), center_y(b), p.px(18), kGray, true);
        right -= p.px(24);
      } else {
        p.signal(right - p.px(10), center_y(b), p.px(22), w.status, w.enabled ? kInk : kFaint,
                 kLine);
        right -= p.px(30);
        if (w.locked) p.lock(right - p.px(8), center_y(b), p.px(20), w.enabled ? kGray : kFaint);
        right -= p.px(24);
      }
      float width = right - left;
      p.text(p.fit(w.label, width, p.px(13.5f)), left, b.y + b.h * .36f, p.px(13.5f), ink);
      p.text(p.fit(w.detail, width, p.px(10.5f)), left, b.y + b.h * .7f, p.px(10.5f),
             w.enabled ? kGray : kFaint);
      break;
    }
    case Kind::kKey: {
      bool special = w.index < 0;
      gpu::draw_rect(p.r, b, p.px(8), down ? kKeyPressed : special ? kKeySpecial : kKey);
      float x = center_x(b), y = center_y(b);
      if (w.index == kShiftKey) p.shift(x, y, p.px(24), w.status, w.status ? kBlue : kInk);
      else if (w.index == kDeleteKey) p.backspace(x, y, p.px(24), kInk);
      else {
        float cap = w.label.size() > 1 ? p.px(10.5f) : p.px(15);
        p.text(w.label, x, y, cap, kInk, 0);
      }
      break;
    }
    case Kind::kField: {
      gpu::draw_rect(p.r, b, p.px(12), kBlue);
      Rect inner{b.x + p.px(2), b.y + p.px(2), b.w - p.px(4), b.h - p.px(4)};
      gpu::draw_rect(p.r, inner, p.px(10), kWhite);
      float left = b.x + p.px(16), limit = b.w - p.px(36), cy = center_y(b), end = left;
      if (w.locked) {
        float step = p.px(13);
        std::size_t count = std::min(w.label.size(), std::size_t(std::max(0.f, limit / step)));
        for (std::size_t i = 0; i < count; ++i) p.dot(left + step * (i + .5f), cy, p.px(4), kInk);
        end = left + step * count;
      } else {
        std::string shown = w.label;
        while (!shown.empty() && p.width(shown, p.px(14)) > limit) shown.erase(shown.begin());
        p.text(shown, left, cy, p.px(14), kInk);
        end = left + p.width(shown, p.px(14));
      }
      if (std::fmod(time, 1.) < .6)
        gpu::draw_rect(p.r, {end + p.px(2), cy - p.px(12), p.px(2), p.px(24)}, 0, kBlue);
      break;
    }
    case Kind::kSpinner:
      p.spinner(b, time, kBlue);
      break;
    case Kind::kStep: {
      float x = b.x + p.px(16), y = b.y + p.px(18);
      if (w.status == 0) {
        p.dot(x, y, p.px(11), kLine);
        p.dot(x, y, p.px(9), kWhite);
      } else if (w.status == 1) {
        p.spinner({x - p.px(13), y - p.px(13), p.px(26), p.px(26)}, time, kBlue);
      } else {
        p.dot(x, y, p.px(11), w.status == 2 ? kGreen : kRed);
        if (w.status == 2) p.check(x, y, p.px(20), kWhite);
        else p.cross(x, y, p.px(20), kWhite);
      }
      float left = b.x + p.px(44), width = b.w - p.px(44);
      p.text(p.fit(w.label, width, p.px(12.5f)), left, y, p.px(12.5f),
             w.status == 0 ? kGray : kInk);
      if (!w.detail.empty())
        p.text(p.fit(w.detail, width, p.px(10.5f)), left, y + p.px(22), p.px(10.5f), kGray);
      break;
    }
  }
}
}

Screen screen_for(const Snapshot& s, bool entering_password) {
  switch (s.stage) {
    case Stage::kSearching:
    case Stage::kDevices:
      return Screen::kDevices;
    case Stage::kHotspot:
    case Stage::kScanning:
      return Screen::kWorking;
    case Stage::kNetworks:
      return entering_password ? Screen::kPassword : Screen::kNetworks;
    case Stage::kFailed:
      return s.target.empty() ? Screen::kResult : Screen::kProgress;
    default:
      return Screen::kProgress;
  }
}

Layout layout_view(int width, int height, Rect content, float density, const ViewInput& in) {
  Layout l;
  l.screen = {0, 0, float(width), float(height)};
  l.safe = content.w > 0 && content.h > 0 ? content : l.screen;
  if (!in.snapshot) return l;
  float s = density;
  // Short windows, such as landscape or split screen, shrink everything.
  float fit = std::min(1.f, l.safe.h / s / minimum_height(in.screen));
  s *= fit;
  l.scale = s;
  float w = l.safe.w / s, h = l.safe.h / s;
  float column = std::max(1.f, std::min(w - 32, 560.f));
  Builder b{l, l.safe.x, l.safe.y, s, (w - column) * .5f, column, h};
  l.list = {};
  switch (in.screen) {
    case Screen::kDevices:
      layout_devices(b, in);
      break;
    case Screen::kWorking:
      layout_working(b, in);
      break;
    case Screen::kNetworks:
      layout_networks(b, in);
      break;
    case Screen::kPassword:
      layout_password(b, in);
      break;
    case Screen::kProgress:
      layout_progress(b, in);
      break;
    case Screen::kResult:
      layout_result(b, in);
      break;
  }
  return l;
}

Hit hit_test(const Layout& l, float x, float y) {
  for (auto it = l.widgets.rbegin(); it != l.widgets.rend(); ++it) {
    const Widget& w = *it;
    if (w.target == Target::kNone || !gpu::contains(w.bounds, x, y)) continue;
    if (w.scrolls && !gpu::contains(l.list, x, y)) continue;
    if (!w.enabled) return {};
    return {w.target, w.index};
  }
  return {};
}

void draw_view(Renderer& r, const Layout& l, Hit pressed, double time) {
  Painter p{r, l.scale, gpu::measure_text(r, "H", 100).h / 100};
  for (const Widget& w : l.widgets)
    if (w.scrolls) draw_widget(p, w, pressed, time);
  // Fixed widgets cover rows scrolled past the list's edges.
  if (l.list.h > 0) {
    gpu::draw_rect(r, {0, 0, l.screen.w, l.list.y}, 0, kWhite);
    float bottom = l.list.y + l.list.h;
    gpu::draw_rect(r, {0, bottom, l.screen.w, l.screen.h - bottom}, 0, kWhite);
    gpu::draw_rect(r, {0, bottom, l.screen.w, std::max(1.f, p.px(.5f))}, 0, kLine);
  }
  for (const Widget& w : l.widgets)
    if (!w.scrolls) draw_widget(p, w, pressed, time);
}

char key_char(int code, const Keyboard& k) {
  if (code >= 'a' && code <= 'z' && k.page == 0 && k.shift) return char(code - 'a' + 'A');
  return code > 0 && code < 127 ? char(code) : 0;
}

std::string printable(const std::string& text) {
  std::string out;
  for (std::size_t i = 0; i < text.size();) {
    unsigned char c = text[i];
    if (c >= 32 && c < 127) {
      out += char(c);
      ++i;
      continue;
    }
    out += '?';
    std::size_t length = c >= 0xf0 ? 4 : c >= 0xe0 ? 3 : c >= 0xc0 ? 2 : 1;
    i += std::min(length, text.size() - i);
  }
  return out;
}
}
