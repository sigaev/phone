#include "sudoku/view.h"

#include <algorithm>
#include <cmath>
#include <cstdio>

#include "common/gpu/renderer.h"

namespace sudoku {
using gpu::Color;
using gpu::Rect;
using gpu::Renderer;

namespace {
// Sudoku.com's palette.
constexpr Color kWhite{1, 1, 1};
constexpr Color kNavy{.204f, .282f, .380f};
constexpr Color kBlue{.196f, .353f, .686f};
constexpr Color kBluePressed{.141f, .267f, .557f};
constexpr Color kGray{.431f, .486f, .549f};
constexpr Color kLine{.745f, .776f, .831f};
constexpr Color kPeer{.886f, .922f, .953f};
constexpr Color kSame{.765f, .843f, .918f};
constexpr Color kSelected{.733f, .871f, .984f};
constexpr Color kRed{.898f, .361f, .424f};
constexpr Color kConflict{.969f, .812f, .839f};
constexpr Color kPad{.918f, .933f, .957f};
constexpr Color kPadPressed{.827f, .863f, .914f};
constexpr Color kBadge{.678f, .714f, .761f};
constexpr Color kScrim{.075f, .110f, .161f, .45f};

constexpr float kMinimumBoard = 240;

Color mix(Color a, Color b, float t) {
  return {a.r + (b.r - a.r) * t, a.g + (b.g - a.g) * t, a.b + (b.b - a.b) * t, 1};
}

bool related(int a, int b) {
  return a != b &&
         (a / 9 == b / 9 || a % 9 == b % 9 || (a / 27 == b / 27 && a % 9 / 3 == b % 9 / 3));
}

float center_x(Rect r) { return r.x + r.w * .5f; }

float center_y(Rect r) { return r.y + r.h * .5f; }

// Rectangles in dp relative to an origin, converted to window pixels.
struct Frame {
  float x, y, s;

  Rect at(float left, float top, float width, float height) const {
    return {x + left * s, y + top * s, width * s, height * s};
  }
};

void place_board(Layout& l, float x, float y, float side) {
  float s = l.scale;
  l.thin = std::max(1.f, std::round(s * .6f));
  l.thick = std::max(2.f, std::round(s * 1.6f));
  l.cell = std::max(1.f, std::floor((side - 6 * l.thin - 4 * l.thick) / 9));
  float size = 9 * l.cell + 6 * l.thin + 4 * l.thick;
  l.board = {std::round(x + (side - size) * .5f), std::round(y + (side - size) * .5f), size, size};
}

// Rows from top to bottom: title and New Game, game information, board,
// actions, and a row of digit keys. Rows span the content width even when a
// short window limits the board.
void layout_portrait(Layout& l, float density) {
  constexpr float kMargin = 12, kHeader = 52, kInfo = 48, kActions = 80, kKey = 64;
  constexpr float kTop = 4, kBottom = 12, kGapBoard = 2, kGapActions = 16, kGapKeys = 12;
  constexpr float kFixed =
      kTop + kHeader + kInfo + kGapBoard + kGapActions + kActions + kGapKeys + kKey + kBottom;
  Rect safe = l.safe;
  float s = density;
  float fit = std::min(
      {1.f, safe.h / s / (kFixed + kMinimumBoard), safe.w / s / (kMinimumBoard + 2 * kMargin)});
  s *= fit;
  l.scale = s;
  float w = safe.w / s, h = safe.h / s;
  float row = std::max(1.f, std::min(w - 2 * kMargin, 560.f));
  float board = std::max(1.f, std::min(row, h - kFixed));
  float extra = std::max(0.f, h - kFixed - board);
  float key = kKey + std::min(extra * .2f, 24.f);
  float gap_board = kGapBoard + std::min(extra * .1f, 16.f);
  float gap_actions = kGapActions + std::min(extra * .2f, 40.f);
  float gap_keys = kGapKeys + std::min(extra * .15f, 28.f);
  float used = kFixed - kKey - kGapBoard - kGapActions - kGapKeys + board + key + gap_board +
               gap_actions + gap_keys;
  float x = (w - row) * .5f, y = kTop + (h - used) * .4f;
  Frame f{safe.x, safe.y, s};
  l.title = f.at(x, y, row * .5f, kHeader);
  l.new_game = f.at(x + row - 128, y + 4, 128, kHeader - 8);
  y += kHeader;
  l.info = f.at(x, y, row, kInfo);
  l.pause = f.at(x + row - 48, y, 48, kInfo);
  y += kInfo + gap_board;
  place_board(l, safe.x + (w - board) * .5f * s, safe.y + y * s, board * s);
  y += board + gap_actions;
  for (int i = 0; i < 3; ++i) l.actions[i] = f.at(x + i * row / 3, y, row / 3, kActions);
  y += kActions + gap_keys;
  float gap = 6, width = (row - 8 * gap) / 9;
  for (int i = 0; i < 9; ++i) l.digits[i] = f.at(x + i * (width + gap), y, width, key);
}

// The board fills the height; the controls stack in a panel beside it.
bool layout_landscape(Layout& l, float density) {
  constexpr float kMargin = 16, kGap = 32, kPanelMinimum = 300, kPanelMaximum = 460;
  constexpr float kHeader = 52, kInfo = 48, kActions = 80, kKey = 64;
  Rect safe = l.safe;
  float s = density;
  float w = safe.w / s, h = safe.h / s;
  float board = std::min(h - 2 * kMargin, 620.f);
  float panel = std::min(kPanelMaximum, w - board - 2 * kMargin - kGap);
  if (panel < kPanelMinimum) {
    board = w - 2 * kMargin - kGap - kPanelMinimum;
    panel = kPanelMinimum;
  }
  if (board < kMinimumBoard || board < kHeader + kInfo + kActions + kKey + 24) return false;
  l.scale = s;
  float x = (w - board - kGap - panel) * .5f, top = (h - board) * .5f;
  place_board(l, safe.x + x * s, safe.y + top * s, board * s);
  // Taller panels use a three-by-three keypad.
  bool grid = board >= kHeader + kInfo + kActions + 3 * 64 + 16 + 48;
  float keys = grid ? std::min(3 * 80.f + 16, board - kHeader - kInfo - kActions - 48)
                    : std::min(80.f, board - kHeader - kInfo - kActions - 24);
  float spare = board - kHeader - kInfo - kActions - keys;
  float gap = std::min(spare / 3, 32.f);
  float px = x + board + kGap, y = top + (spare - 3 * gap) * .5f;
  Frame f{safe.x, safe.y, s};
  l.title = f.at(px, y, panel * .5f, kHeader);
  l.new_game = f.at(px + panel - 128, y + 4, 128, kHeader - 8);
  y += kHeader;
  l.info = f.at(px, y, panel, kInfo);
  l.pause = f.at(px + panel - 48, y, 48, kInfo);
  y += kInfo + gap;
  for (int i = 0; i < 3; ++i) l.actions[i] = f.at(px + i * panel / 3, y, panel / 3, kActions);
  y += kActions + gap;
  if (grid) {
    float spacing = 8, key = std::min((panel - 2 * spacing) / 3, 128.f);
    float height = (keys - 2 * spacing) / 3;
    float left = px + (panel - 3 * key - 2 * spacing) * .5f;
    for (int i = 0; i < 9; ++i)
      l.digits[i] =
          f.at(left + i % 3 * (key + spacing), y + i / 3 * (height + spacing), key, height);
  } else {
    float spacing = 6, key = (panel - 8 * spacing) / 9;
    for (int i = 0; i < 9; ++i) l.digits[i] = f.at(px + i * (key + spacing), y, key, keys);
  }
  return true;
}

void layout_dialog(Layout& l) {
  float s = l.scale;
  float width = std::min(l.safe.w / s - 32, 340.f), height = 256;
  Frame f{l.safe.x + (l.safe.w - width * s) * .5f, l.safe.y + (l.safe.h - height * s) * .5f, s};
  l.dialog = f.at(0, 0, width, height);
  l.primary = f.at(24, 144, width - 48, 48);
  l.secondary = f.at(24, 198, width - 48, 44);
}

float cell_left(const Layout& l, int column) {
  float offset = l.thick + column * l.cell;
  for (int k = 1; k <= column; ++k) offset += k % 3 ? l.thin : l.thick;
  return offset;
}

struct Painter {
  Renderer& r;
  float s;
  // Capital height per unit of text height.
  float cap;

  float px(float dp) const { return dp * s; }

  // Center capitals and digits vertically on cy.
  void text(const char* value, float x, float cy, float cap_height, Color color,
            int align = 0) const {
    float height = cap_height / cap, baseline = cy + cap_height * .5f;
    if (align > 0) x -= gpu::measure_text(r, value, height).w;
    gpu::draw_text(r, value, x, baseline, height, color, align == 0);
  }

  float width(const char* value, float cap_height) const {
    return gpu::measure_text(r, value, cap_height / cap).w;
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
    int segments = std::max(4, int(std::fabs(to - from) / (gpu::kPi / 12)));
    float x = cx + std::cos(from) * radius, y = cy + std::sin(from) * radius;
    for (int i = 1; i <= segments; ++i) {
      float angle = from + (to - from) * i / segments;
      float nx = cx + std::cos(angle) * radius, ny = cy + std::sin(angle) * radius;
      line(x, y, nx, ny, thickness, color);
      x = nx;
      y = ny;
    }
  }
};

// Icons are drawn in an i-by-i box centered on (x, y).
void undo_icon(const Painter& p, float x, float y, float i, Color color) {
  float thickness = i * .11f, top = y - i * .16f, bottom = y + i * .3f;
  float bend = x + i * .1f, radius = (bottom - top) * .5f;
  p.line(x - i * .24f, top, bend, top, thickness, color);
  p.arc(bend, (top + bottom) * .5f, radius, -gpu::kPi * .5f, gpu::kPi * .5f, thickness, color);
  p.line(bend, bottom, x - i * .16f, bottom, thickness, color);
  gpu::draw_triangle(p.r, x - i * .46f, top, x - i * .2f, top - i * .23f, x - i * .2f,
                     top + i * .23f, color);
}

struct Axes {
  float x, y, i;

  // u runs up and to the right, v down and to the right.
  void at(float u, float v, float& px, float& py) const {
    px = x + (u + v) * .7071f * i;
    py = y + (v - u) * .7071f * i;
  }
};

void quad(const Painter& p, const Axes& a, float u0, float u1, float v0, float v1, Color color) {
  float x[4], y[4];
  a.at(u0, v0, x[0], y[0]);
  a.at(u1, v0, x[1], y[1]);
  a.at(u1, v1, x[2], y[2]);
  a.at(u0, v1, x[3], y[3]);
  gpu::draw_triangle(p.r, x[0], y[0], x[1], y[1], x[2], y[2], color);
  gpu::draw_triangle(p.r, x[0], y[0], x[2], y[2], x[3], y[3], color);
}

void erase_icon(const Painter& p, float x, float y, float i, Color color) {
  Axes a{x - i * .02f, y - i * .08f, i};
  float thickness = i * .09f, corners[4][2];
  a.at(-.36f, -.18f, corners[0][0], corners[0][1]);
  a.at(.36f, -.18f, corners[1][0], corners[1][1]);
  a.at(.36f, .18f, corners[2][0], corners[2][1]);
  a.at(-.36f, .18f, corners[3][0], corners[3][1]);
  for (int k = 0; k < 4; ++k)
    p.line(corners[k][0], corners[k][1], corners[(k + 1) % 4][0], corners[(k + 1) % 4][1],
           thickness, color);
  quad(p, a, -.36f, -.04f, -.18f, .18f, color);
  p.line(x - i * .06f, y + i * .38f, x + i * .4f, y + i * .38f, thickness, color);
}

void pencil_icon(const Painter& p, float x, float y, float i, Color color) {
  Axes a{x + i * .02f, y - i * .02f, i};
  quad(p, a, -.14f, .28f, -.13f, .13f, color);
  quad(p, a, .35f, .46f, -.13f, .13f, color);
  float x0, y0, x1, y1, x2, y2;
  a.at(-.21f, -.13f, x0, y0);
  a.at(-.21f, .13f, x1, y1);
  a.at(-.46f, 0, x2, y2);
  gpu::draw_triangle(p.r, x0, y0, x1, y1, x2, y2, color);
}

void pause_icon(const Painter& p, float x, float y, float i, Color color) {
  float width = i * .15f, height = i * .52f;
  gpu::draw_rect(p.r, {x - i * .2f, y - height * .5f, width, height}, width * .3f, color);
  gpu::draw_rect(p.r, {x + i * .05f, y - height * .5f, width, height}, width * .3f, color);
}

void play_icon(const Painter& p, float x, float y, float i, Color color) {
  gpu::draw_triangle(p.r, x - i * .17f, y - i * .27f, x - i * .17f, y + i * .27f, x + i * .29f, y,
                     color);
}

void format_time(double seconds, char* text, std::size_t size) {
  int total = int(seconds);
  if (total >= 3600)
    std::snprintf(text, size, "%d:%02d:%02d", total / 3600, total / 60 % 60, total % 60);
  else std::snprintf(text, size, "%02d:%02d", total / 60, total % 60);
}

bool pressed(const ViewState& v, Target target, int index = -1) {
  return v.pressed.target == target && v.pressed.index == index;
}

void draw_header(const Painter& p, const Layout& l, const Board* b, const ViewState& v) {
  p.text("Sudoku", l.title.x, center_y(l.title), p.px(19), kNavy, -1);
  Rect button{l.new_game.x, l.new_game.y + p.px(3), l.new_game.w, l.new_game.h - p.px(6)};
  gpu::draw_rect(p.r, button, button.h * .5f, pressed(v, Target::kNewGame) ? kBluePressed : kBlue);
  p.text("New Game", center_x(button), center_y(button), p.px(11), kWhite);
  float cy = center_y(l.info), cap = p.px(10.5f);
  p.text("Extreme", l.info.x, cy, cap, kGray, -1);
  char count[8];
  int mistakes = b ? b->mistakes : 0;
  std::snprintf(count, sizeof(count), "%d/%d", mistakes, kMaximumMistakes);
  float label = p.width("Mistakes: ", cap), total = label + p.width(count, cap);
  float left = center_x(l.info) - total * .5f;
  p.text("Mistakes: ", left, cy, cap, kGray, -1);
  p.text(count, left + label, cy, cap, mistakes ? kRed : kGray, -1);
  char time[16];
  format_time(b ? b->elapsed : 0, time, sizeof(time));
  bool playing = b && b->status == Status::kPlaying;
  float time_right = playing ? l.pause.x + p.px(4) : l.info.x + l.info.w;
  p.text(time, time_right, cy, cap, kGray, 1);
  if (playing) {
    float x = center_x(l.pause), y = center_y(l.pause);
    p.dot(x, y, p.px(16), pressed(v, Target::kPause) ? kPadPressed : kPad);
    if (v.paused) play_icon(p, x + p.px(1), y, p.px(20), kBlue);
    else pause_icon(p, x, y, p.px(20), kBlue);
  }
}

void draw_board(const Painter& p, const Layout& l, const Board* b, const ViewState& v) {
  Renderer& r = p.r;
  bool visible = b && !v.paused;
  int selected = visible ? b->selected : -1;
  int value = selected >= 0 ? b->cells[selected].value : 0;
  bool wrong = selected >= 0 && b->cells[selected].wrong;
  if (visible)
    for (int cell = 0; cell < kCells; ++cell) {
      Color color = kWhite;
      if (cell == selected) color = kSelected;
      else if (value && b->cells[cell].value == value)
        color = wrong && related(cell, selected) ? kConflict : kSame;
      else if (selected >= 0 && related(cell, selected)) color = kPeer;
      if (v.flash_age >= 0 && v.flash_cell >= 0) {
        int units[3] = {cell / 9, 9 + cell % 9, 18 + cell / 27 * 3 + cell % 9 / 3};
        if ((v.flash_units >> units[0] & 1) || (v.flash_units >> units[1] & 1) ||
            (v.flash_units >> units[2] & 1)) {
          int distance = std::max(std::abs(cell / 9 - v.flash_cell / 9),
                                  std::abs(cell % 9 - v.flash_cell % 9));
          float t = (v.flash_age - distance * .06f) / .3f;
          if (t > 0 && t < 1) color = mix(color, kSelected, std::sin(t * gpu::kPi) * .85f);
        }
      }
      if (color.r != 1 || color.g != 1 || color.b != 1)
        gpu::draw_rect(r, cell_bounds(l, cell), 0, color);
    }
  Rect board = l.board;
  for (bool thick : {false, true})
    for (int k = 1; k < 9; ++k) {
      if ((k % 3 == 0) != thick) continue;
      float width = thick ? l.thick : l.thin, offset = cell_left(l, k) - width;
      Color color = thick ? kNavy : kLine;
      gpu::draw_rect(r, {board.x + offset, board.y, width, board.h}, 0, color);
      gpu::draw_rect(r, {board.x, board.y + offset, board.w, width}, 0, color);
    }
  float t = l.thick;
  gpu::draw_rect(r, {board.x, board.y, board.w, t}, 0, kNavy);
  gpu::draw_rect(r, {board.x, board.y + board.h - t, board.w, t}, 0, kNavy);
  gpu::draw_rect(r, {board.x, board.y, t, board.h}, 0, kNavy);
  gpu::draw_rect(r, {board.x + board.w - t, board.y, t, board.h}, 0, kNavy);
  if (visible)
    for (int cell = 0; cell < kCells; ++cell) {
      const Cell& c = b->cells[cell];
      Rect bounds = cell_bounds(l, cell);
      if (c.value) {
        char digit[2] = {char('0' + c.value), 0};
        Color color = c.given ? kNavy : c.wrong ? kRed : kBlue;
        p.text(digit, center_x(bounds), center_y(bounds), bounds.h * .46f, color);
      } else if (c.notes) {
        float third = bounds.w / 3;
        for (int digit = 1; digit <= 9; ++digit) {
          if (!(c.notes & (1u << (digit - 1)))) continue;
          char mark[2] = {char('0' + digit), 0};
          float x = bounds.x + ((digit - 1) % 3 + .5f) * third;
          float y = bounds.y + ((digit - 1) / 3 + .5f) * third;
          p.text(mark, x, y, third * .5f, digit == value && !wrong ? kBlue : kGray);
        }
      }
    }
  if (b && v.paused) {
    float radius = std::min(p.px(36), board.w * .12f);
    p.dot(center_x(board), center_y(board), radius,
          pressed(v, Target::kResume) ? kBluePressed : kBlue);
    play_icon(p, center_x(board) + radius * .08f, center_y(board), radius * 1.1f, kWhite);
  }
  if (!b) p.text("Generating puzzle...", center_x(board), center_y(board), p.px(11), kGray);
}

void draw_actions(const Painter& p, const Layout& l, const Board* b, const ViewState& v) {
  const char* labels[] = {"Undo", "Erase", "Notes"};
  Target targets[] = {Target::kUndo, Target::kErase, Target::kNotes};
  bool active = b && !v.paused && b->status == Status::kPlaying;
  for (int i = 0; i < 3; ++i) {
    Rect bounds = l.actions[i];
    float x = center_x(bounds), y = bounds.y + p.px(28), icon = p.px(26);
    bool enabled = active && (i != 0 || b->can_undo);
    Color color = enabled ? kBlue : mix(kBlue, kWhite, .55f);
    p.dot(x, y, p.px(26), pressed(v, targets[i]) && enabled ? kPadPressed : kPad);
    if (i == 0) undo_icon(p, x, y, icon, color);
    else if (i == 1) erase_icon(p, x, y, icon, color);
    else pencil_icon(p, x, y, icon, color);
    p.text(labels[i], x, bounds.y + p.px(67), p.px(9.5f), color);
    if (i == 2) {
      bool on = b && b->notes_mode;
      Rect badge{x + p.px(8), bounds.y - p.px(1), p.px(on ? 28 : 32), p.px(18)};
      Rect ring{badge.x - p.px(2), badge.y - p.px(2), badge.w + p.px(4), badge.h + p.px(4)};
      gpu::draw_rect(p.r, ring, ring.h * .5f, kWhite);
      gpu::draw_rect(p.r, badge, badge.h * .5f, on ? kBlue : kBadge);
      p.text(on ? "ON" : "OFF", center_x(badge), center_y(badge), p.px(7), kWhite);
    }
  }
}

void draw_keys(const Painter& p, const Layout& l, const Board* b, const ViewState& v) {
  bool active = b && !v.paused && b->status == Status::kPlaying;
  Color digit_color = !active ? mix(kBlue, kWhite, .55f) : b->notes_mode ? kGray : kBlue;
  for (int digit = 1; digit <= 9; ++digit) {
    if (b && b->placed[digit] >= 9) continue;
    Rect bounds = l.digits[digit - 1];
    bool down = active && pressed(v, Target::kDigit, digit);
    gpu::draw_rect(p.r, bounds, std::min(p.px(10), bounds.w * .25f), down ? kPadPressed : kPad);
    char text[2] = {char('0' + digit), 0};
    float cap = std::min({bounds.h * .42f, bounds.w * .6f, p.px(30)});
    p.text(text, center_x(bounds), center_y(bounds), cap, digit_color);
  }
}

void draw_dialog(const Painter& p, const Layout& l, const Board* b, const ViewState& v) {
  gpu::draw_rect(p.r, l.screen, 0, kScrim);
  Rect card = l.dialog;
  gpu::draw_rect(p.r, card, p.px(18), kWhite);
  const char *title = "", *first = "", *primary = "New Game", *secondary = "Close";
  char second[48] = "";
  if (v.dialog == Dialog::kConfirmNewGame) {
    title = "Start a new game?";
    first = "Your current progress";
    std::snprintf(second, sizeof(second), "will be lost.");
    secondary = "Cancel";
  } else if (v.dialog == Dialog::kLost) {
    title = "Game Over";
    first = "You have made 3 mistakes";
    std::snprintf(second, sizeof(second), "and lost this game.");
  } else {
    char time[16];
    format_time(b ? b->elapsed : 0, time, sizeof(time));
    title = "Excellent!";
    first = "You solved an Extreme puzzle";
    std::snprintf(second, sizeof(second), "in %s.", time);
  }
  float x = center_x(card);
  p.text(title, x, card.y + p.px(50), p.px(15), kNavy);
  p.text(first, x, card.y + p.px(88), p.px(10.5f), kGray);
  p.text(second, x, card.y + p.px(112), p.px(10.5f), kGray);
  Rect button = l.primary;
  gpu::draw_rect(p.r, button, button.h * .5f, pressed(v, Target::kPrimary) ? kBluePressed : kBlue);
  p.text(primary, center_x(button), center_y(button), p.px(11.5f), kWhite);
  if (pressed(v, Target::kSecondary)) gpu::draw_rect(p.r, l.secondary, l.secondary.h * .5f, kPad);
  p.text(secondary, center_x(l.secondary), center_y(l.secondary), p.px(11.5f), kBlue);
}
}

Layout layout_view(int width, int height, Rect content, float density) {
  Layout l;
  l.screen = {0, 0, float(width), float(height)};
  if (content.w <= 0 || content.h <= 0) content = l.screen;
  float left = std::clamp(content.x, 0.f, float(width));
  float top = std::clamp(content.y, 0.f, float(height));
  l.safe = {left, top, std::clamp(content.x + content.w, left, float(width)) - left,
            std::clamp(content.y + content.h, top, float(height)) - top};
  float s = std::isfinite(density) && density > 0 ? density : 1;
  float w = l.safe.w / s, h = l.safe.h / s;
  l.landscape = w > h * 1.15f && layout_landscape(l, s);
  if (!l.landscape) layout_portrait(l, s);
  layout_dialog(l);
  return l;
}

Rect cell_bounds(const Layout& l, int cell) {
  return {l.board.x + cell_left(l, cell % 9), l.board.y + cell_left(l, cell / 9), l.cell, l.cell};
}

Hit hit_test(const Layout& l, const Board* b, const ViewState& v, float x, float y) {
  auto inside = [&](Rect r) { return gpu::contains(r, x, y); };
  if (v.dialog != Dialog::kNone) {
    if (inside(l.primary)) return {Target::kPrimary};
    if (inside(l.secondary)) return {Target::kSecondary};
    return {};
  }
  if (inside(l.new_game)) return {Target::kNewGame};
  if (!b) return {};
  bool playing = b->status == Status::kPlaying;
  if (playing && inside(l.pause)) return {Target::kPause};
  if (v.paused) return inside(l.board) ? Hit{Target::kResume} : Hit{};
  if (inside(l.board)) {
    int column = std::clamp(int((x - l.board.x) / l.board.w * 9), 0, 8);
    int row = std::clamp(int((y - l.board.y) / l.board.h * 9), 0, 8);
    return {Target::kCell, row * 9 + column};
  }
  Target actions[] = {Target::kUndo, Target::kErase, Target::kNotes};
  for (int i = 0; i < 3; ++i)
    if (inside(l.actions[i])) return {actions[i]};
  for (int digit = 1; digit <= 9; ++digit)
    if (b->placed[digit] < 9 && inside(l.digits[digit - 1])) return {Target::kDigit, digit};
  return {};
}

void draw_view(Renderer& r, const Layout& l, const Board* b, const ViewState& v) {
  Painter p{r, l.scale, gpu::measure_text(r, "H", 100).h / 100};
  draw_header(p, l, b, v);
  draw_board(p, l, b, v);
  draw_actions(p, l, b, v);
  draw_keys(p, l, b, v);
  if (v.dialog != Dialog::kNone) draw_dialog(p, l, b, v);
}
}
