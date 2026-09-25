#include <algorithm>
#include <cmath>
#include <cstdio>
#include <numbers>
#include <string>
#include <vector>

#include "common/gpu/renderer.h"
#include "native_buttons/controls.h"
#include "native_buttons/scene.h"

// Record the actual scene's submitted geometry without a GPU. This catches
// scenery leaving its wrapping range, independently of a rendered image's UI.
namespace gpu {
struct Renderer {
  unsigned next_mesh = 7;
  int width = 400, height = 720;
  bool defer_present = false;
  std::vector<float> islands, rocks;
  Vec3 eye;
  std::vector<std::string> labels;
  std::vector<MeshId> meshes;
};

void clear_instances(Renderer& r) {
  r.islands.clear();
  r.rocks.clear();
  r.labels.clear();
  r.meshes.clear();
}

void set_transform(Renderer&, Mat4) {}

void add(Renderer& r, Shape, Mat4 model, Color color, float, float, float, float) {
  if (color.r == .37f && color.g == .32f && color.b == .18f) r.islands.push_back(model.v[12]);
  if (color.r == .21f && color.g == .27f && color.b == .26f) r.rocks.push_back(model.v[12]);
}

void add(Renderer& r, MeshId mesh, Mat4, Color, float, float, float, float) {
  r.meshes.push_back(mesh);
}

common::Result<MeshId> create_mesh(Renderer& r, std::span<const Vertex> vertices,
                                   std::span<const unsigned> indices) {
  for (auto index : indices)
    if (index >= vertices.size())
      return std::unexpected(common::Error{"Scene mesh index is out of bounds"});
  return r.next_mesh++;
}

common::Result<bool> prepare_frame(Renderer&, bool) { return true; }

common::Result<bool> render(Renderer& r, Vec3 eye, Vec3, double, bool) {
  if (!std::isfinite(eye.x) || !std::isfinite(eye.y) || !std::isfinite(eye.z))
    return std::unexpected(common::Error{"Invalid camera"});
  r.eye = eye;
  return true;
}

common::Result<bool> present(Renderer& r) { return !r.defer_present; }

void draw_rect(Renderer&, Rect, float, Color) {}

void draw_text(Renderer& r, const char* text, float, float, float, Color, bool) {
  r.labels.emplace_back(text);
}

RenderStats get_stats(const Renderer& r) {
  return {r.width, r.height, r.width, r.height, 4, 16384, 0, 0, false};
}

std::string_view get_device(const Renderer&) { return "scene-test"; }
}

int main() {
  gpu::Renderer renderer;
  auto scene = native_buttons::create_scene(renderer);
  if (!scene) return 1;
  for (bool maximum : {false, true}) {
    for (double time : {0., 76.9, 80., 105., 120., 3600., 86400., 524288., 31536000.}) {
      auto rendered = native_buttons::render_scene(**scene, time, .34f, maximum, 0, 0, 60, false,
                                                   {0, 0, 400, 720});
      if (!rendered || !*rendered) return 2;
      auto in_range = [](float x) { return x >= -22 && x < 22; };
      if (renderer.islands.size() != (maximum ? 16u : 9u) || renderer.rocks.size() != 24 ||
          !std::all_of(renderer.islands.begin(), renderer.islands.end(), in_range) ||
          !std::all_of(renderer.rocks.begin(), renderer.rocks.end(), in_range)) {
        std::fprintf(stderr, "Scenery escaped its loop at %.1f seconds\n", time);
        return 3;
      }
    }
  }
  // Changing birds reuses prebuilt meshes and updates every species label.
  auto mesh_count = renderer.next_mesh;
  gpu::MeshId previous_neck = 0;
  for (auto bird : {native_buttons::Bird::kPelican, native_buttons::Bird::kFlamingo,
                    native_buttons::Bird::kPelican}) {
    auto rendered = native_buttons::render_scene(**scene, 3.28, .34f, false, 0, 0, 60, true, {},
                                                 true, true, 1, 1, bird);
    bool flamingo = bird == native_buttons::Bird::kFlamingo;
    const char* name = flamingo ? "Flamingo" : "Pelican";
    const char* caption =
        flamingo ? "A flamingo. A bicycle. No hurry." : "A pelican. A bicycle. No hurry.";
    const char* other = flamingo ? "Pelican" : "Flamingo";
    if (!rendered || !*rendered || renderer.next_mesh != mesh_count || renderer.meshes.empty() ||
        renderer.meshes.front() == previous_neck ||
        std::find(renderer.labels.begin(), renderer.labels.end(), name) == renderer.labels.end() ||
        std::find(renderer.labels.begin(), renderer.labels.end(), caption) ==
            renderer.labels.end() ||
        std::find(renderer.labels.begin(), renderer.labels.end(), other) != renderer.labels.end())
      return 14;
    previous_neck = renderer.meshes.front();
  }
  // A single frame must still move scenery and camera after days or a year,
  // including frames spanning either periodic clock's wrap boundary.
  for (double time : {65536., 262144., 524288., 31536000., 200 * std::numbers::pi * 1000 - 1. / 120,
                      600000. - 1. / 120}) {
    auto first = native_buttons::render_scene(**scene, time, .34f, false, 0, 0, 60, false, {});
    if (!first || !*first) return 10;
    auto islands = renderer.islands;
    auto eye = renderer.eye;
    auto second =
        native_buttons::render_scene(**scene, time + 1. / 60, .34f, false, 0, 0, 60, false, {});
    if (!second || !*second) return 11;
    for (size_t i = 0; i < islands.size(); ++i) {
      float moved = gpu::wrap(renderer.islands[i] - islands[i] + 22.f, 44.f) - 22.f;
      if (std::abs(moved + 2.6 / 60) > .00001) return 12;
    }
    float camera_moved = gpu::length(renderer.eye - eye);
    if (camera_moved < .001f || camera_moved > .1f) return 13;
  }

  const struct {
    float x, y;
    int expected;
  } taps[] = {{100, 650, 1}, {310, 650, 2}, {60, 150, 3}, {180, 150, 4},
              {300, 150, 5}, {1, 1, 0},     {260, 650, 0}};

  for (auto tap : taps)
    if (native_buttons::hit_test(**scene, tap.x, tap.y) != tap.expected) return 4;
  renderer.width = 720;
  renderer.height = 320;
  renderer.defer_present = true;
  auto deferred = native_buttons::render_scene(**scene, 0, .34f, false, 0, 0, 0, true, {});
  if (!deferred || *deferred || native_buttons::hit_test(**scene, 100, 650) != 1) return 5;
  renderer.defer_present = false;
  auto resized = native_buttons::render_scene(**scene, 0, .34f, false, 0, 0, 0, true, {});
  if (!resized || !*resized || native_buttons::hit_test(**scene, 100, 650) != 0 ||
      native_buttons::hit_test(**scene, 523, 248) != 1)
    return 6;
  // Exercise real physical densities, both orientations, insets and short/narrow
  // windows. Controls must remain usable, disjoint and inside the safe rectangle.
  for (float density : {1.f, 2.625f, 3.f, 4.f}) {
    for (auto size :
         {gpu::Rect{0, 0, 360, 800}, gpu::Rect{0, 0, 800, 360}, gpu::Rect{0, 0, 720, 200},
          gpu::Rect{0, 0, 200, 300}, gpu::Rect{0, 0, 200, 800}, gpu::Rect{0, 0, 360, 160},
          gpu::Rect{0, 0, 200, 200}, gpu::Rect{0, 0, 300, 420}, gpu::Rect{0, 0, 160, 200},
          gpu::Rect{0, 0, 160, 152}, gpu::Rect{0, 0, 160, 320}, gpu::Rect{0, 0, 480, 320},
          gpu::Rect{0, 0, 560, 280}}) {
      gpu::Rect safe{10 * density, 24 * density, size.w * density, size.h * density};
      auto layout = native_buttons::layout_overlay(safe, density);
      auto panel = layout.panel;
      if (panel.x < safe.x || panel.y < safe.y || panel.x + panel.w > safe.x + safe.w ||
          panel.y + panel.h > safe.y + safe.h)
        return 15;
      auto c = layout.controls;
      const gpu::Rect buttons[] = {c.add, c.reset, c.quality, c.pause, c.bird};
      for (int i = 0; i < 5; ++i) {
        auto a = buttons[i];
        if (a.w < 48 * density || a.h < 48 * density || a.x < safe.x || a.y < safe.y ||
            a.x + a.w > safe.x + safe.w || a.y + a.h > safe.y + safe.h)
          return 7;
        if (static_cast<int>(native_buttons::hit_test(c, a.x + a.w * .5f, a.y + a.h * .5f)) !=
            i + 1)
          return 8;
        for (int j = i + 1; j < 5; ++j) {
          auto b = buttons[j];
          if (a.x < b.x + b.w && b.x < a.x + a.w && a.y < b.y + b.h && b.y < a.y + a.h) return 9;
        }
      }
    }
  }
  std::puts(
      "Long-running scenery/camera motion, phase wraps, control hit regions and meshes passed");
  return 0;
}
