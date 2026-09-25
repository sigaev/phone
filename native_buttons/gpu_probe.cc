// Render and benchmark the app's actual scene on the local GPU before installing.

#include <algorithm>
#include <chrono>
#if defined(NATIVE_BUTTONS_VULKAN_VALIDATION)
#include <dlfcn.h>
#include <string>
#endif
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <numbers>
#include <vector>

#if defined(NATIVE_BUTTONS_TEST_DEPTH_FALLBACK)
#include <vulkan/vulkan.h>
extern "C" VkResult __real_vkGetPhysicalDeviceImageFormatProperties(VkPhysicalDevice, VkFormat,
                                                                    VkImageType, VkImageTiling,
                                                                    VkImageUsageFlags,
                                                                    VkImageCreateFlags,
                                                                    VkImageFormatProperties*);

extern "C" VkResult __wrap_vkGetPhysicalDeviceImageFormatProperties(
    VkPhysicalDevice physical, VkFormat format, VkImageType type, VkImageTiling tiling,
    VkImageUsageFlags usage, VkImageCreateFlags flags, VkImageFormatProperties* properties) {
  // Simulate a valid device without D24S8. All other capabilities and rendering
  // still use the real driver, including the fallback depth/shadow images.
  if (format == VK_FORMAT_D24_UNORM_S8_UINT) return VK_ERROR_FORMAT_NOT_SUPPORTED;
  return __real_vkGetPhysicalDeviceImageFormatProperties(physical, format, type, tiling, usage,
                                                         flags, properties);
}
#endif

#include "common/gpu/renderer.h"
#include "native_buttons/scene.h"

#if defined(NATIVE_BUTTONS_VULKAN_VALIDATION)
namespace gpu {
unsigned validation_error_count();
}
#endif

namespace {
using native_buttons::Bird;

common::Result<void> require_frame(common::Result<bool> result) {
  if (!result) return std::unexpected(result.error());
  if (!*result)
    return std::unexpected(common::Error{"The offscreen frame was unexpectedly deferred"});
  return {};
}
#if defined(NATIVE_BUTTONS_VULKAN_VALIDATION)
common::Result<void> prepare_validation() {
  const char* path = std::getenv("NATIVE_BUTTONS_VULKAN_LAYER_PATH");
  if (!path) return std::unexpected(common::Error{"Use //tools:vulkan_validation_runner"});
  // The CLI has no Activity to configure Android's layer search path. This
  // test-only bootstrap uses the platform GraphicsEnv; it is absent from the APK.
  using GetEnvironment = void* (*)();
  using SetPaths = void (*)(void*, void*, const std::string&);
  auto get_environment = reinterpret_cast<GetEnvironment>(
      dlsym(RTLD_DEFAULT, "_ZN7android11GraphicsEnv11getInstanceEv"));
  auto set_paths = reinterpret_cast<SetPaths>(
      dlsym(RTLD_DEFAULT,
            "_ZN7android11GraphicsEnv13setLayerPathsEPNS_21NativeLoaderNamespaceERKNSt3__"
            "112basic_stringIcNS3_11char_traitsIcEENS3_9allocatorIcEEEE"));
  if (!get_environment || !set_paths)
    return std::unexpected(common::Error{"Android's validation-layer bootstrap is unavailable"});
  set_paths(get_environment(), nullptr, std::string(path));
  return {};
}
#endif

common::Result<void> exercise_renderer() {
  for (bool landscape : {false, true}) {
    int width = landscape ? 720 : 320, height = landscape ? 320 : 720;
    auto renderer =
        gpu::create_renderer(nullptr, native_buttons::get_scene_shaders(), width, height);
    if (!renderer) return std::unexpected(renderer.error());
    auto scene = native_buttons::create_scene(**renderer);
    if (!scene) return std::unexpected(scene.error());
    // A different quality request must defer instead of replacing targets
    // after the caller has calculated its camera and layout.
    auto before = gpu::get_stats(**renderer);
    auto changed = gpu::render(**renderer, {0, 3, 8}, {0, 1, 0}, 0, true);
    if (!changed) return std::unexpected(changed.error());
    if (*changed) return std::unexpected(common::Error{"Rendering replaced unprepared targets"});
    auto after = gpu::get_stats(**renderer);
    if (after.render_width != before.render_width || after.render_height != before.render_height)
      return std::unexpected(common::Error{"Deferred rendering changed target dimensions"});
    std::vector<unsigned char> pixels(size_t(width) * height * 4);
    for (int i = 0; i < 12; ++i) {
      bool maximum = i >= 4 && i < 8;
      Bird bird = i % 4 < 2 ? Bird::kPelican : Bird::kFlamingo;
      auto rendered = require_frame(native_buttons::render_scene(
          **scene, 2.f + i * .25f, .34f, maximum, 7, 0, 60, false,
          {0, 0, float(width), float(height)}, true, true, 1, 1, bird));
      if (!rendered) return rendered;
      if (i % 2 == 1) {
        if (auto captured = gpu::read_pixels(**renderer, pixels); !captured) return captured;
        int darkest = 255, brightest = 0;
        // Translucent controls and font edges must preserve opaque scene alpha.
        for (size_t byte = 0; byte < pixels.size(); ++byte)
          if (byte % 4 != 3) {
            darkest = std::min(darkest, int(pixels[byte]));
            brightest = std::max(brightest, int(pixels[byte]));
          } else if (pixels[byte] != 255) {
            return std::unexpected(common::Error{"UI made the opaque scene transparent"});
          }
        if (brightest - darkest < 100)
          return std::unexpected(common::Error{"Rendered scene is blank"});
        auto stats = gpu::get_stats(**renderer);
        if (stats.samples != 4 || stats.particles != (maximum ? 65536 : 16384) ||
            stats.triangles != (bird == Bird::kPelican ? (maximum ? 869830u : 747078u)
                                                       : (maximum ? 862150u : 739398u)))
          return std::unexpected(common::Error{"Scene workload changed unexpectedly"});
      }
    }
    // Compare fixed-quality frames with all UI (including changing GPU timings)
    // excluded. Equal timestamps must reproduce an image; advancing time must
    // change it. Long timestamps also exercise the scenery's wrapping logic.
    for (bool maximum : {false, true}) {
      auto capture = [&](double time, float zoom = 1,
                         Bird bird = Bird::kFlamingo) -> common::Result<void> {
        if (auto rendered = require_frame(native_buttons::render_scene(
                **scene, time, .34f, maximum, 7, 0, 60, false, {0, 0, float(width), float(height)},
                true, false, 1, zoom, bird));
            !rendered)
          return rendered;
        return gpu::read_pixels(**renderer, pixels);
      };
      if (auto result = capture(120.f); !result) return result;
      auto reference = pixels;
      if (auto result = capture(120.f); !result) return result;
      if (pixels != reference)
        return std::unexpected(common::Error{"Frozen scene changed without input"});
      if (auto result = capture(120.f, 1, Bird::kPelican); !result) return result;
      if (pixels == reference)
        return std::unexpected(common::Error{"Bird selector did not change the scene"});
      auto pelican = pixels;
      if (auto result = capture(120.5f, 1, Bird::kPelican); !result) return result;
      if (pixels == pelican) return std::unexpected(common::Error{"Pelican animation is frozen"});
      if (auto result = capture(120.f); !result) return result;
      if (pixels != reference)
        return std::unexpected(common::Error{"Switching birds changed the frozen scene"});
      for (float zoom : {native_buttons::kMinimumZoom, native_buttons::kMaximumZoom}) {
        if (auto result = capture(120.f, zoom); !result) return result;
        size_t changed = 0;
        for (size_t byte = 0; byte < pixels.size(); ++byte)
          changed += pixels[byte] != reference[byte];
        if (changed < pixels.size() / 1000)
          return std::unexpected(common::Error{"Pinch zoom did not change the rendered camera"});
      }
      if (auto result = capture(120.5f); !result) return result;
      size_t changed = 0;
      for (size_t byte = 0; byte < pixels.size(); ++byte)
        changed += pixels[byte] != reference[byte];
      if (changed < pixels.size() / 1000)
        return std::unexpected(common::Error{"Fixed-quality scene animation is frozen"});

      for (double time : {524288., 31536000.}) {
        if (auto result = capture(time); !result) return result;
        reference = pixels;
        if (auto result = capture(time + 1. / 60); !result) return result;
        if (pixels == reference)
          return std::unexpected(common::Error{"Long-running scene stopped animating"});
      }
    }

    // Fix the camera and geometry so only shader-driven water and particles
    // can change. This catches narrowing the clock again at the GPU boundary.
    auto capture_shader_motion = [&](double time) -> common::Result<void> {
      if (auto prepared = require_frame(gpu::prepare_frame(**renderer, false)); !prepared)
        return prepared;
      gpu::clear_instances(**renderer);
      gpu::add(**renderer, gpu::Shape::kPlane, gpu::scale({20, 1, 20}), {.02f, .2f, .24f}, .2f, .4f,
               0, 1);
      if (auto rendered = require_frame(gpu::render(**renderer, {0, 6, 8}, {0, 0, 0}, time, false));
          !rendered)
        return rendered;
      if (auto presented = require_frame(gpu::present(**renderer)); !presented) return presented;
      return gpu::read_pixels(**renderer, pixels);
    };
    for (double time : {524288., 31536000., 200 * std::numbers::pi * 1000 - 1. / 120}) {
      if (auto result = capture_shader_motion(time); !result) return result;
      auto reference = pixels;
      if (auto result = capture_shader_motion(time); !result) return result;
      if (pixels != reference)
        return std::unexpected(common::Error{"Frozen shader animation changed"});
      if (auto result = capture_shader_motion(time + 1. / 60); !result) return result;
      size_t changed = 0, difference = 0;
      for (size_t byte = 0; byte < pixels.size(); ++byte) {
        changed += pixels[byte] != reference[byte];
        difference += std::abs(int(pixels[byte]) - int(reference[byte]));
      }
      if (changed < pixels.size() / 1000 || difference > pixels.size() * 3)
        return std::unexpected(common::Error{"Long-running shader motion froze or jumped"});
    }
  }
  std::puts(
      "Vulkan recreation, quality transitions, async frames, fixed-quality animation and "
      "readback passed");
  return {};
}

common::Result<void> export_video() {
  constexpr int kWidth = 720;
  constexpr int kHeight = 1600;
  constexpr int kFrameRate = 30;
  constexpr int kFrameCount = 10 * kFrameRate;
  auto renderer =
      gpu::create_renderer(nullptr, native_buttons::get_scene_shaders(), kWidth, kHeight);
  if (!renderer) return std::unexpected(renderer.error());
  auto scene = native_buttons::create_scene(**renderer);
  if (!scene) return std::unexpected(scene.error());
  std::fprintf(stderr, "Exporting 10 seconds from %s at %dx%d, %d fps\n",
               gpu::get_device(**renderer).data(), kWidth, kHeight, kFrameRate);
  std::vector<unsigned char> pixels(kWidth * kHeight * 4);
  float fps = 0;
  for (int i = 0; i < kFrameCount; ++i) {
    auto begin = std::chrono::steady_clock::now();
    auto result = require_frame(native_buttons::render_scene(
        **scene, 2.f + float(i) / kFrameRate, .34f, false, 0, 0, fps, false,
        {0, 48.f, float(kWidth), kHeight - 96.f}, true, true, 2));
    if (!result) return std::unexpected(result.error());
    if (auto waited = gpu::wait_frame(**renderer); !waited) return std::unexpected(waited.error());
    float elapsed = std::chrono::duration<float>(std::chrono::steady_clock::now() - begin).count();
    fps = i == 0 ? 1.f / elapsed : fps * .85f + .15f / elapsed;
    if (auto captured = gpu::read_pixels(**renderer, pixels); !captured)
      return std::unexpected(captured.error());
    // Stream top-down RGBA to the encoder without storing raw frames on disk.
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
  double start_time = argc > 5 ? std::strtod(argv[5], nullptr) : 2.;
  float density = argc > 6 ? std::strtof(argv[6], nullptr) : std::min(width, height) / 360.f;
  float zoom = argc > 7 ? std::strtof(argv[7], nullptr) : 1;
  if (argc > 8 && std::strcmp(argv[8], "pelican") != 0 && std::strcmp(argv[8], "flamingo") != 0)
    return std::unexpected(common::Error{"Bird must be pelican or flamingo"});
  Bird bird = argc > 8 && std::strcmp(argv[8], "pelican") == 0 ? Bird::kPelican : Bird::kFlamingo;
  if (width < 1 || height < 1 || width > 4096 || height > 4096)
    return std::unexpected(common::Error{"Invalid image dimensions"});
  if (!std::isfinite(start_time) || start_time < 0)
    return std::unexpected(common::Error{"Invalid animation start time"});
  if (!std::isfinite(density) || density <= 0 || height <= 48 * density)
    return std::unexpected(common::Error{"Invalid display density"});
  if (!std::isfinite(zoom) || zoom < native_buttons::kMinimumZoom ||
      zoom > native_buttons::kMaximumZoom)
    return std::unexpected(common::Error{"Invalid camera zoom"});
  auto renderer = gpu::create_renderer(nullptr, native_buttons::get_scene_shaders(), width, height);
  if (!renderer) return std::unexpected(renderer.error());
  auto scene = native_buttons::create_scene(**renderer);
  if (!scene) return std::unexpected(scene.error());
  std::printf("Renderer: %s\n", gpu::get_device(**renderer).data());
  std::fflush(stdout);
  auto begin = std::chrono::steady_clock::now(), previous = begin;
  float fps = 0;
  for (int i = 0; i < 90; ++i) {
    double time = start_time + i / 60.;
    auto result = require_frame(native_buttons::render_scene(
        **scene, time, .34f, maximum, 7, 0, fps, false,
        {0, 24 * density, float(width), height - 48 * density}, true, true, density, zoom, bird));
    if (!result) return std::unexpected(result.error());
    if (auto waited = gpu::wait_frame(**renderer); !waited) return std::unexpected(waited.error());
    auto now = std::chrono::steady_clock::now();
    float instantaneous = 1.f / std::chrono::duration<float>(now - previous).count();
    fps = i == 0 ? instantaneous : fps * .85f + instantaneous * .15f;
    previous = now;
    if (i == 29) begin = now;
  }
  double seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - begin).count();
  auto stats = gpu::get_stats(**renderer);
  std::printf("Size: %dx%d; scene: %dx%d; MSAA: %d; particles: %d; triangles/color pass: %u\n",
              width, height, stats.render_width, stats.render_height, stats.samples,
              stats.particles, stats.triangles);
  if (stats.has_gpu_timer && stats.gpu_ms > 0) std::printf("GPU: %.2f ms\n", stats.gpu_ms);
  else std::printf("Driver GPU timer queries unavailable\n");
  std::printf("Synchronous throughput: %.2f fps\n", 60 / seconds);
  if (argc > 1) return gpu::capture_frame(**renderer, argv[1]);
  return {};
}
}

int main(int argc, char** argv) {
#if defined(NATIVE_BUTTONS_VULKAN_VALIDATION)
  if (auto result = prepare_validation(); !result) {
    std::fprintf(stderr, "%s\n", result.error().message.c_str());
    return 1;
  }
#endif
  auto result = argc > 1 && std::strcmp(argv[1], "--exercise") == 0 ? exercise_renderer()
                : argc > 1 && std::strcmp(argv[1], "--video") == 0  ? export_video()
                                                                    : run_probe(argc, argv);
  if (!result) {
    std::fprintf(stderr, "%s\n", result.error().message.c_str());
    return 1;
  }
#if defined(NATIVE_BUTTONS_VULKAN_VALIDATION)
  if (gpu::validation_error_count()) return 1;
#endif
  return 0;
}
