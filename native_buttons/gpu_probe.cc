// Render and benchmark the app's actual scene on the local GPU before installing.
#include <GLES3/gl32.h>

#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>

#include "common/gpu/renderer.h"
#include "native_buttons/scene.h"

namespace {
common::Result<void> run_probe(int argc, char** argv) {
    int width = argc > 2 ? std::atoi(argv[2]) : 1080;
    int height = argc > 3 ? std::atoi(argv[3]) : 2400;
    bool maximum = argc > 4 && std::atoi(argv[4]) != 0;
    float start_time = argc > 5 ? std::strtof(argv[5], nullptr) : 2.f;
    if (width < 1 || height < 1 || width > 4096 || height > 4096)
        return std::unexpected(common::Error{"Invalid image dimensions"});
    if (!std::isfinite(start_time) || start_time < 0)
        return std::unexpected(common::Error{"Invalid animation start time"});
    auto renderer =
        gpu::create_renderer(nullptr, native_buttons::get_scene_shaders(), width, height);
    if (!renderer)
        return std::unexpected(renderer.error());
    auto scene = native_buttons::create_scene(**renderer);
    if (!scene)
        return std::unexpected(scene.error());
    std::printf("Renderer: %s\n", gpu::get_device(**renderer).data());
    std::fflush(stdout);
    auto begin = std::chrono::steady_clock::now(), previous = begin;
    float fps = 0;
    for (int i = 0; i < 90; ++i) {
        float time = start_time + i / 60.f;
        auto result = native_buttons::render_scene(**scene, time, .34f, maximum, 7, 0, fps, false,
                                                   {0, 45.f, float(width), height - 85.f});
        if (!result)
            return std::unexpected(result.error());
        glFinish();
        auto now = std::chrono::steady_clock::now();
        float instantaneous = 1.f / std::chrono::duration<float>(now - previous).count();
        fps = i == 0 ? instantaneous : fps * .85f + instantaneous * .15f;
        previous = now;
        if (i == 29)
            begin = now;
    }
    double seconds =
        std::chrono::duration<double>(std::chrono::steady_clock::now() - begin).count();
    auto stats = gpu::get_stats(**renderer);
    std::printf("Size: %dx%d; scene: %dx%d; MSAA: %d; particles: %d; triangles/color pass: %u\n",
                width, height, stats.render_width, stats.render_height, stats.samples,
                stats.particles, stats.triangles);
    if (stats.has_gpu_timer && stats.gpu_ms > 0)
        std::printf("GPU: %.2f ms\n", stats.gpu_ms);
    else
        std::printf("Driver GPU timer queries unavailable\n");
    std::printf("Synchronous throughput: %.2f fps\n", 60 / seconds);
    if (argc > 1)
        return gpu::capture_frame(**renderer, argv[1]);
    return {};
}
}  // namespace

int main(int argc, char** argv) {
    auto result = run_probe(argc, argv);
    if (!result) {
        std::fprintf(stderr, "%s\n", result.error().message.c_str());
        return 1;
    }
    return 0;
}
