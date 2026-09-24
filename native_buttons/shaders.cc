#include "native_buttons/scene.h"

#include "common/gpu/renderer.h"

namespace native_buttons {
namespace {
constexpr std::uint32_t kMeshVertex[] =
#include "native_buttons/mesh_vert.inc"
    ;
constexpr std::uint32_t kSceneFragment[] =
#include "native_buttons/scene_frag.inc"
    ;
constexpr std::uint32_t kSkyFragment[] =
#include "native_buttons/sky_frag.inc"
    ;
}  // namespace

gpu::SceneShaders get_scene_shaders() {
    return {kMeshVertex, kSceneFragment, kSkyFragment};
}
}  // namespace native_buttons
