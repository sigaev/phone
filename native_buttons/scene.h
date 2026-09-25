#pragma once

#include "common/owner.h"
#include "common/result.h"

namespace gpu {
struct Renderer;
struct Rect;
struct SceneShaders;
}  // namespace gpu
namespace native_buttons {
struct Scene;
enum class Bird { kPelican, kFlamingo };
inline constexpr float kMinimumZoom = .5f, kMaximumZoom = 2.5f;
gpu::SceneShaders get_scene_shaders();
common::Result<common::Owner<Scene>> create_scene(gpu::Renderer& renderer);
void destroy(Scene* scene) noexcept;
// Test the controls from the last successfully presented frame.
int hit_test(const Scene& scene, float x, float y);
// Returns false when rendering must be retried after a transient surface change.
// The content rectangle is clipped to the single prepared frame's dimensions.
common::Result<bool> render_scene(Scene& scene, double time, float yaw, bool maximum, int count,
                                  int pressed, float fps, bool paused, gpu::Rect content,
                                  bool saved = true, bool overlay = true, float density = 1,
                                  float zoom = 1, Bird bird = Bird::kFlamingo);
}  // namespace native_buttons
