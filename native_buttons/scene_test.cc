#include <algorithm>
#include <cmath>
#include <cstdio>
#include <vector>

#include "common/gpu/renderer.h"
#include "native_buttons/scene.h"

// Record the actual scene's submitted geometry without a GPU. This catches
// scenery leaving its wrapping range, independently of a rendered image's UI.
namespace gpu {
struct Renderer {
    unsigned next_mesh = 7;
    int width = 400, height = 720;
    std::vector<float> islands, rocks;
};
void clear_instances(Renderer& r) {
    r.islands.clear();
    r.rocks.clear();
}
void set_transform(Renderer&, Mat4) {
}
void add(Renderer& r, Shape, Mat4 model, Color color, float, float, float, float) {
    if (color.r == .37f && color.g == .32f && color.b == .18f)
        r.islands.push_back(model.v[12]);
    if (color.r == .21f && color.g == .27f && color.b == .26f)
        r.rocks.push_back(model.v[12]);
}
void add(Renderer&, MeshId, Mat4, Color, float, float, float, float) {
}
common::Result<MeshId> create_mesh(Renderer& r, std::span<const Vertex> vertices,
                                   std::span<const unsigned> indices) {
    for (auto index : indices)
        if (index >= vertices.size())
            return std::unexpected(common::Error{"Scene mesh index is out of bounds"});
    return r.next_mesh++;
}
common::Result<void> prepare_frame(Renderer&, bool) {
    return {};
}
common::Result<void> render(Renderer&, Vec3 eye, Vec3, float, bool) {
    if (!std::isfinite(eye.x) || !std::isfinite(eye.y) || !std::isfinite(eye.z))
        return std::unexpected(common::Error{"Invalid camera"});
    return {};
}
common::Result<void> present(Renderer&) {
    return {};
}
void draw_rect(Renderer&, Rect, float, Color) {
}
void draw_text(Renderer&, const char*, float, float, float, Color, bool) {
}
RenderStats get_stats(const Renderer& r) {
    return {r.width, r.height, r.width, r.height, 4, 16384, 0, 0, false};
}
std::string_view get_device(const Renderer&) {
    return "scene-test";
}
}  // namespace gpu

int main() {
    gpu::Renderer renderer;
    auto scene = native_buttons::create_scene(renderer);
    if (!scene)
        return 1;
    for (bool maximum : {false, true}) {
        for (float time : {0.f, 76.9f, 80.f, 105.f, 120.f, 3600.f, 86400.f}) {
            if (!native_buttons::render_scene(**scene, time, .34f, maximum, 0, 0, 60, false,
                                              {0, 0, 400, 720}))
                return 2;
            auto in_range = [](float x) { return x >= -22 && x < 22; };
            if (renderer.islands.size() != (maximum ? 16u : 9u) || renderer.rocks.size() != 24 ||
                !std::all_of(renderer.islands.begin(), renderer.islands.end(), in_range) ||
                !std::all_of(renderer.rocks.begin(), renderer.rocks.end(), in_range)) {
                std::fprintf(stderr, "Scenery escaped its loop at %.1f seconds\n", time);
                return 3;
            }
        }
    }
    const struct {
        float x, y;
        int expected;
    } taps[] = {{100, 650, 1}, {310, 650, 2}, {60, 150, 3},
                {180, 150, 4}, {1, 1, 0},     {260, 650, 0}};
    for (auto tap : taps)
        if (native_buttons::hit_test(**scene, tap.x, tap.y) != tap.expected)
            return 4;
    std::puts("Actual scenery wraps through 24 hours; control hit regions and meshes passed");
    return 0;
}
