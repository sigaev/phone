#pragma once

#include <string>
#include <vector>

#include "chromecast/session.h"
#include "common/gpu/math.h"

namespace gpu {
struct Renderer;
}

namespace chromecast {
enum class Screen { kDevices, kWorking, kNetworks, kPassword, kProgress, kResult };
enum class Target {
  kNone,
  kBack,
  kDevice,
  kHotspot,
  kSearch,
  kNetwork,
  kRescan,
  kReveal,
  kPaste,
  kConnect,
  kKey,
  kRetry,
  kDone
};
// Keyboard keys use their ASCII code as the index; these are the others.
inline constexpr int kShiftKey = -1, kDeleteKey = -2, kSymbolsKey = -3, kMoreKey = -4,
                     kLettersKey = -5;

struct Keyboard {
  // 0 letters, 1 symbols, 2 more symbols.
  int page = 0;
  // 0 lowercase, 1 one capital, 2 capitals until shift is tapped again.
  int shift = 0;
};

struct Hit {
  Target target = Target::kNone;
  int index = -1;
};

enum class Kind {
  kTitle,
  kSubtitle,
  kText,
  kError,
  kSuccess,
  kButton,
  kPrimary,
  kRow,
  kKey,
  kField,
  kLink,
  kSpinner,
  kStep
};

struct Widget {
  Kind kind = Kind::kText;
  Target target = Target::kNone;
  int index = -1;
  gpu::Rect bounds;
  std::string label, detail;
  // Device rows: 0 on Wi-Fi, 1 setup hotspot, 2 out of reach. Network rows:
  // signal bars 1-4. Steps: 0 pending, 1 active, 2 done, 3 failed.
  int status = 0;
  // Field: whether the password is hidden. Network rows: secured.
  bool enabled = true, locked = false;
  // Part of the scrolling list.
  bool scrolls = false;
};

struct ViewInput {
  Screen screen = Screen::kDevices;
  const Snapshot* snapshot = nullptr;
  // The network chosen for the password screen.
  int chosen = -1;
  std::string password;
  bool reveal = false;
  Keyboard keyboard;
  float scroll = 0;
};

struct Layout {
  gpu::Rect screen, safe;
  // The scrolling list's viewport and the height of its contents.
  gpu::Rect list;
  float list_height = 0, scale = 1;
  std::vector<Widget> widgets;
};

Screen screen_for(const Snapshot& snapshot, bool entering_password);
// Lay out a width x height window. Widgets stay inside the content rectangle,
// which excludes system bars and cutouts; an empty one selects the window.
Layout layout_view(int width, int height, gpu::Rect content, float density, const ViewInput& input);
Hit hit_test(const Layout& layout, float x, float y);
void draw_view(gpu::Renderer& renderer, const Layout& layout, Hit pressed, double time);
// The character a printable key types; letters follow shift.
char key_char(int code, const Keyboard& keyboard);
// Replace bytes the font cannot draw, one '?' per UTF-8 character.
std::string printable(const std::string& text);
}
