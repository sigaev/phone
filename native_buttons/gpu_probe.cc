// Render and benchmark the app's actual scene on the local GPU before installing.
#include <GLES3/gl32.h>

#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

#include "common/gpu/renderer.h"
#include "native_buttons/scene.h"

namespace {
common::Result<void> export_video() {
    constexpr int kWidth = 720;
    constexpr int kHeight = 1600;
    constexpr int kFrameRate = 30;
    constexpr int kFrameCount = 10 * kFrameRate;
    auto renderer =
        gpu::create_renderer(nullptr, native_buttons::get_scene_shaders(), kWidth, kHeight);
    if (!renderer)
        return std::unexpected(renderer.error());
    auto scene = native_buttons::create_scene(**renderer);
    if (!scene)
        return std::unexpected(scene.error());
    std::fprintf(stderr, "Exporting 10 seconds from %s at %dx%d, %d fps\n",
                 gpu::get_device(**renderer).data(), kWidth, kHeight, kFrameRate);
    std::vector<unsigned char> pixels(kWidth * kHeight * 4);
    float fps = 0;
    for (int i = 0; i < kFrameCount; ++i) {
        auto begin = std::chrono::steady_clock::now();
        auto result =
            native_buttons::render_scene(**scene, 2.f + float(i) / kFrameRate, .34f, false, 0, 0,
                                         fps, false, {0, 45.f, float(kWidth), kHeight - 85.f});
        if (!result)
            return std::unexpected(result.error());
        glFinish();
        float elapsed =
            std::chrono::duration<float>(std::chrono::steady_clock::now() - begin).count();
        fps = i == 0 ? 1.f / elapsed : fps * .85f + .15f / elapsed;
        glReadPixels(0, 0, kWidth, kHeight, GL_RGBA, GL_UNSIGNED_BYTE, pixels.data());
        if (glGetError() != GL_NO_ERROR)
            return std::unexpected(common::Error{"Cannot capture the GPU frame"});
        // Stream bottom-up RGBA to the encoder without storing raw frames on disk.
        if (std::fwrite(pixels.data(), 1, pixels.size(), stdout) != pixels.size())
            return std::unexpected(common::Error{"Cannot write the video frame"});
        if ((i + 1) % kFrameRate == 0)
            std::fprintf(stderr, "Exported %d / 10 seconds\n", (i + 1) / kFrameRate);
    }
    if (std::fflush(stdout) != 0)
        return std::unexpected(common::Error{"Cannot finish the video stream"});
    return {};
}

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
    auto result =
        argc > 1 && std::strcmp(argv[1], "--video") == 0 ? export_video() : run_probe(argc, argv);
    if (!result) {
        std::fprintf(stderr, "%s\n", result.error().message.c_str());
        return 1;
    }
    return 0;
}
