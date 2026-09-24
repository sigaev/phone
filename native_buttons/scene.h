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
gpu::SceneShaders get_scene_shaders();
common::Result<common::Owner<Scene>> create_scene(gpu::Renderer& renderer);
void destroy(Scene* scene) noexcept;
int hit_test(const Scene& scene, float x, float y);
common::Result<void> render_scene(Scene& scene, float time, float yaw, bool maximum, int count,
                                  int pressed, float fps, bool paused, gpu::Rect safe,
                                  bool saved = true, bool overlay = true);
}  // namespace native_buttons
