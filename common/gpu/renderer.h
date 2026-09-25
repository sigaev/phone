#pragma once

#include <cstdint>
#include <span>
#include <string_view>

#include "common/gpu/math.h"
#include "common/owner.h"
#include "common/result.h"

struct ANativeWindow;

namespace gpu {
struct Renderer;

struct SceneShaders {
  std::span<const std::uint32_t> vertex;
  std::span<const std::uint32_t> fragment;
  std::span<const std::uint32_t> sky;
};
enum class Shape { kSphere, kLowSphere, kTorus, kCylinder, kCone, kPlane, kFeather, kCount };
using MeshId = unsigned;

struct Vertex {
  Vec3 position, normal;
};

struct RenderStats {
  int width, height, render_width, render_height, samples, particles;
  unsigned triangles;
  float gpu_ms;
  bool has_gpu_timer;
};

common::Result<common::Owner<Renderer>> create_renderer(ANativeWindow* window, SceneShaders shaders,
                                                        int offscreen_width = 0,
                                                        int offscreen_height = 0);
void destroy(Renderer* renderer) noexcept;
common::Result<MeshId> create_mesh(Renderer& renderer, std::span<const Vertex> vertices,
                                   std::span<const unsigned> indices);
void clear_instances(Renderer& renderer);
void set_transform(Renderer& renderer, Mat4 transform);
void add(Renderer& renderer, Shape shape, Mat4 model, Color color, float roughness = .45f,
         float metal = 0, float emission = 0, float kind = 0);
void add(Renderer& renderer, MeshId mesh, Mat4 model, Color color, float roughness = .45f,
         float metal = 0, float emission = 0, float kind = 0);
// Inspect the Vulkan extent/transform and independent native-window dimensions
// without allocating targets or rendering.
// Idle windows must keep checking: Android can resize after its last callback.
common::Result<bool> surface_changed(const Renderer& renderer);
// Frame operations return false when the surface is temporarily unavailable.
// Retry from prepare_frame on a later frame; do not continue to render/present.
// Prepare once before constructing the camera and UI layout. render() preserves
// these dimensions, deferring the frame if the window changes in the meantime.
common::Result<bool> prepare_frame(Renderer& renderer, bool maximum);
common::Result<bool> render(Renderer& renderer, Vec3 eye, Vec3 target, double time, bool maximum);
void draw_rect(Renderer& renderer, Rect bounds, float radius, Color color);
void draw_text(Renderer& renderer, const char* value, float x, float baseline, float height,
               Color color, bool centered = false);
common::Result<bool> present(Renderer& renderer);
common::Result<void> capture_frame(Renderer& renderer, const char* ppm_path);
common::Result<void> wait_frame(Renderer& renderer);
// Capture tightly packed RGBA rows from top to bottom on an offscreen renderer.
common::Result<void> read_pixels(Renderer& renderer, std::span<unsigned char> rgba);
RenderStats get_stats(const Renderer& renderer);
std::string_view get_device(const Renderer& renderer);
}
