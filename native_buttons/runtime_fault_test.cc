#include <android/choreographer.h>
#include <android/input.h>
#include <android/looper.h>
#include <sys/eventfd.h>
#include <sys/resource.h>
#include <sys/timerfd.h>
#include <sys/wait.h>
#include <unistd.h>

#include <atomic>
#include <chrono>
#include <cmath>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <utility>
#include <vector>

#include "common/gpu/renderer.h"
#include "native_buttons/gestures.h"
#include "native_buttons/runtime.h"

struct AInputEvent {
  int action;
  gpu::Vec3 current;
  std::vector<gpu::Vec3> history;
};

// Frame timelines one refresh apart, starting with the preferred timeline.
struct AChoreographerFrameCallbackData {
  int64_t presentation, period;
};

extern "C" {
int32_t AMotionEvent_getAction(const AInputEvent* event) { return event->action; }

size_t AMotionEvent_getPointerCount(const AInputEvent*) { return 1; }

int32_t AMotionEvent_getPointerId(const AInputEvent*, size_t) { return 7; }

float AMotionEvent_getX(const AInputEvent* event, size_t) { return event->current.x; }

float AMotionEvent_getY(const AInputEvent* event, size_t) { return event->current.y; }

size_t AMotionEvent_getHistorySize(const AInputEvent* event) { return event->history.size(); }

float AMotionEvent_getHistoricalX(const AInputEvent* event, size_t, size_t sample) {
  return event->history.at(sample).x;
}

float AMotionEvent_getHistoricalY(const AInputEvent* event, size_t, size_t sample) {
  return event->history.at(sample).y;
}

size_t AChoreographerFrameCallbackData_getFrameTimelinesLength(
    const AChoreographerFrameCallbackData*) {
  return 2;
}

size_t AChoreographerFrameCallbackData_getPreferredFrameTimelineIndex(
    const AChoreographerFrameCallbackData*) {
  return 0;
}

int64_t AChoreographerFrameCallbackData_getFrameTimelineExpectedPresentationTimeNanos(
    const AChoreographerFrameCallbackData* vsync, size_t timeline) {
  return vsync->presentation + int64_t(timeline) * vsync->period;
}

// Like this phone, a frame must be ready two refreshes before its presentation.
int64_t AChoreographerFrameCallbackData_getFrameTimelineDeadlineNanos(
    const AChoreographerFrameCallbackData* vsync, size_t timeline) {
  return vsync->presentation + (int64_t(timeline) - 2) * vsync->period;
}
}

namespace {
std::atomic<int> prepare_deferrals{0}, present_deferrals{0};
std::atomic<bool> unavailable{false}, resize_during_render{false}, renderer_alive{false};
std::atomic<unsigned> waits{0}, callbacks{0};
std::atomic<float> visible_add_x{0}, visible_add_y{0};
std::atomic<bool> stall_render{false}, stall_destroy{false}, worker_stalled{false};
// A consumer resize arrives independently of Activity callbacks and vsync.
std::atomic<std::uint64_t> surface_geometry{0};
std::atomic<unsigned> surface_checks{0};
std::atomic<bool> surface_query_failure{false};
// Frames requested closer together than the simulated GPU frame cost are
// reported late. Also track requested times and their spacing.
std::atomic<int64_t> frame_cost{0}, present_time{0}, present_spacing{0}, simulated_now{0};
std::atomic<unsigned> late_frames{0}, uneven_presents{0}, warmup_frames{0}, presents{0},
    spike_every{0};
std::atomic<int64_t> spike_cost{0};
std::atomic<float> slow_clock{1}, settle_excess{0};
constexpr int64_t kWarmupCost = 16000000;

// The simulated GPU cost of the next frame at the current frame spacing.
int64_t simulated_cost() {
  if (warmup_frames) return kWarmupCost;
  return present_spacing > 12000000 ? int64_t(frame_cost * slow_clock) : frame_cost.load();
}

// The GPU time the runtime measures, which may still be settling.
float measured_cost() { return simulated_cost() * (1 + settle_excess) / 1e6f; }

std::uint64_t geometry(int width, int height) {
  return (std::uint64_t(width) << 32) | unsigned(height);
}

// Deliver deterministic vsync timelines on the real worker's looper, without
// sleeping for animation time or calling the worker recursively.
// Vsyncs at each display rate and simulated GPU frame cost in turn. Like this
// phone, Choreographer may call back on only every so many vsyncs, the GPU may
// start slowly, its clock may fall at slower frame rates, raising the cost, and
// every so many frames may cost more than the average the runtime measures.
struct Mode {
  unsigned callbacks, rate;
  int64_t cost;
  unsigned every = 1, warmup = 0;
  float slow_clock = 1;
  unsigned spike_every = 0;
  int64_t spike = 0;
  // The measured GPU time starts this much higher and settles by 5% of the
  // difference per frame, like this phone's GPU warming up.
  float settle = 0;
};

// Simulated time advances to each vsync callback, just after its deadline for the
// first timeline, or to the frame timer if that fires first. It starts a second
// ahead of the real clock and advances faster, so real time never overtakes it.
struct FrameClock {
  int fd;
  std::vector<Mode> modes;
  unsigned remaining = 0, delivered = 0, rate = 0;
  int64_t presentation = 0, timer = 0;
  int timer_fd = -1;
  AChoreographer_vsyncCallback callback = nullptr;
  void* data = nullptr;
  std::atomic<bool> finished{false};
};

FrameClock* frame_clock = nullptr;

void destroy(FrameClock* clock) noexcept {
  close(clock->fd);
  delete clock;
}

void wake(int fd) {
  const std::uint64_t one = 1;
  if (write(fd, &one, sizeof(one)) != sizeof(one)) std::abort();
}

int deliver_frame(int fd, int, void* data) {
  auto& clock = *static_cast<FrameClock*>(data);
  std::uint64_t signals;
  if (read(fd, &signals, sizeof(signals)) != sizeof(signals)) std::abort();
  if (!clock.remaining) return 1;
  unsigned rate = 0, every = 1, callbacks = clock.delivered;
  int64_t cost = 0;
  for (auto mode : clock.modes)
    if (!rate && callbacks < mode.callbacks) {
      rate = mode.rate;
      cost = mode.cost;
      every = mode.every;
      slow_clock = mode.slow_clock;
      spike_every = mode.spike_every;
      spike_cost = mode.spike;
    } else if (!rate) {
      callbacks -= mode.callbacks;
    }
  int64_t period = 1000000000 / rate, presentation = clock.presentation + period;
  int64_t callback_time = presentation - 2 * period + 1000000;
  if (clock.timer && clock.timer <= callback_time) {
    simulated_now = std::max<int64_t>(simulated_now, clock.timer);
    clock.timer = 0;
    wake(clock.timer_fd);
    return 1;
  }
  if (!clock.callback) return 1;
  // Like this phone, report frames shown late around each display mode change.
  if (clock.rate && rate != clock.rate) late_frames += 3;
  clock.rate = rate;
  frame_cost = cost;
  --clock.remaining;
  ++clock.delivered;
  clock.presentation = presentation;
  simulated_now = std::max(simulated_now.load(), callback_time);
  if (callbacks % every) {
    // A throttled vsync passes without a callback.
    wake(fd);
  } else {
    AChoreographerFrameCallbackData vsync{presentation, period};
    std::exchange(clock.callback, nullptr)(&vsync, clock.data);
  }
  if (!clock.remaining) clock.finished = true;
  return 1;
}

void block_worker() {
  worker_stalled = true;
  for (;;) pause();
}
}

// Run the real scene and worker against a deterministic GPU boundary. In
// particular, reject a second preparation after frame layout has begun.
namespace gpu {
struct Renderer {
  int width, height;
  unsigned next_mesh = 7;
  bool prepared = false, resize_pending = false;
  Rect add;
};

common::Result<common::Owner<Renderer>> create_renderer(ANativeWindow*, SceneShaders, int width,
                                                        int height) {
  renderer_alive = true;
  return common::Owner<Renderer>(new Renderer{width, height});
}

void destroy(Renderer* renderer) noexcept {
  if (stall_destroy) block_worker();
  delete renderer;
  renderer_alive = false;
}

common::Result<MeshId> create_mesh(Renderer& r, std::span<const Vertex>,
                                   std::span<const unsigned>) {
  return r.next_mesh++;
}

void clear_instances(Renderer&) {}

void set_transform(Renderer&, Mat4) {}

void set_present_time(Renderer&, std::int64_t nanoseconds) {
  auto previous = present_time.exchange(nanoseconds);
  int64_t cost =
      spike_every && ++presents % spike_every == 0 ? spike_cost.load() : simulated_cost();
  if (warmup_frames) --warmup_frames;
  settle_excess = settle_excess * .95f;
  if (!previous) return;
  auto spacing = nanoseconds - previous;
  // Like this phone's GPU, consecutive frames overlap enough to sustain a budget
  // 1.2 times below their measured cost. A frame is also late if it started too
  // close to its slot to be ready two refreshes before it, like this phone's.
  int64_t refresh = frame_clock && frame_clock->rate ? 1000000000 / frame_clock->rate : 0;
  if (spacing * 6 / 5 < cost || nanoseconds + 2000000 - simulated_now < 2 * refresh + cost)
    ++late_frames;
  if (present_spacing && std::llabs(spacing - present_spacing) > 1000) ++uneven_presents;
  present_spacing = spacing;
}

unsigned take_late_frames(Renderer&) { return late_frames.exchange(0); }

void add(Renderer&, Shape, Mat4, Color, float, float, float, float) {}

void add(Renderer&, MeshId, Mat4, Color, float, float, float, float) {}

common::Result<bool> surface_changed(const Renderer& r) {
  ++surface_checks;
  if (surface_query_failure) return std::unexpected(common::Error{"Surface query failed"});
  auto extent = surface_geometry.load();
  return extent && extent != geometry(r.width, r.height);
}

common::Result<bool> prepare_frame(Renderer& r, bool) {
  if (r.prepared) return std::unexpected(common::Error{"Frame targets were prepared twice"});
  if (unavailable) return false;
  if (prepare_deferrals > 0) {
    --prepare_deferrals;
    return false;
  }
  if (r.resize_pending) {
    r.width = 720;
    r.height = 320;
    r.resize_pending = false;
  }
  if (auto extent = surface_geometry.load()) {
    int width = extent >> 32, height = unsigned(extent);
    if (!width || !height) return false;
    r.width = width;
    r.height = height;
  }
  r.prepared = true;
  return true;
}

common::Result<bool> render(Renderer& r, Vec3, Vec3, double, bool) {
  if (stall_render) block_worker();
  if (!r.prepared) return std::unexpected(common::Error{"Rendering without prepared targets"});
  if (resize_during_render.exchange(false)) {
    r.resize_pending = true;
    r.prepared = false;
    return false;
  }
  return true;
}

void draw_rect(Renderer& r, Rect bounds, float, Color color) {
  if (color.r == .49f && color.g == .94f && color.b == .76f) r.add = bounds;
}

void draw_text(Renderer&, const char*, float, float, float, Color, bool) {}

common::Result<bool> present(Renderer& r) {
  r.prepared = false;
  if (present_deferrals > 0) {
    --present_deferrals;
    return false;
  }
  visible_add_x = r.add.x + r.add.w * .5f;
  visible_add_y = r.add.y + r.add.h * .5f;
  return true;
}

common::Result<void> wait_frame(Renderer&) {
  ++waits;
  return {};
}

RenderStats get_stats(const Renderer& r) {
  return {r.width, r.height, r.width, r.height, 4, 16384, 0, measured_cost()};
}

std::string_view get_device(const Renderer&) { return "runtime-fault-test"; }
}

// Simulate a display which stops delivering vsync. A required redraw must
// recover or fail without depending on another Choreographer callback.
extern "C" void __wrap_AChoreographer_postVsyncCallback(AChoreographer*,
                                                        AChoreographer_vsyncCallback callback,
                                                        void* data) {
  ++callbacks;
  if (frame_clock && frame_clock->remaining) {
    frame_clock->callback = callback;
    frame_clock->data = data;
    if (ALooper_addFd(ALooper_forThread(), frame_clock->fd, ALOOPER_POLL_CALLBACK,
                      ALOOPER_EVENT_INPUT, deliver_frame, frame_clock) != 1)
      std::abort();
    wake(frame_clock->fd);
  }
}

// The frame timer is an eventfd that the simulated display fires at its target.
extern "C" int __wrap_timerfd_create(int, int) { return eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK); }

extern "C" int __wrap_timerfd_settime(int fd, int, const itimerspec* value, itimerspec*) {
  if (frame_clock) {
    frame_clock->timer_fd = fd;
    frame_clock->timer = int64_t(value->it_value.tv_sec) * 1000000000 + value->it_value.tv_nsec;
    wake(frame_clock->fd);
  }
  return 0;
}

extern "C" int __real_clock_gettime(clockid_t, timespec*);

// While the display is simulated, CLOCK_MONOTONIC follows its time.
extern "C" int __wrap_clock_gettime(clockid_t id, timespec* time) {
  int result = __real_clock_gettime(id, time);
  int64_t simulated = simulated_now;
  if (!result && id == CLOCK_MONOTONIC && frame_clock &&
      simulated > int64_t(time->tv_sec) * 1000000000 + time->tv_nsec)
    *time = {time_t(simulated / 1000000000), long(simulated % 1000000000)};
  return result;
}

namespace {
using common::Error;
using common::Result;
using namespace native_buttons;
#define CHECK(condition, message)                             \
  do {                                                        \
    if (!(condition)) return std::unexpected(Error{message}); \
  } while (false)

enum class Stall { kSnapshot, kDetach, kDestroy, kRedrawThenDestroy };

Result<void> check_idle_resize(const char* directory) {
  auto runtime = create_runtime(
      directory, SessionState{.count = 41, .paused = true, .yaw = .7f, .time = 123., .zoom = 1.2f});
  CHECK(runtime, "Cannot create paused resize runtime");
  auto& r = **runtime;
  CHECK(set_surface(r, nullptr, 400, 720), "Cannot attach paused resize scene");
  set_visible(r, true);
  set_resumed(r, true);
  set_content(r, {});
  CHECK(redraw(r), "Cannot complete the early rotation callback");
  // Android's new consumer extent arrives after the last Activity callback.
  // No input, redraw request or Choreographer callback follows this change.
  auto wait_for_extent = [&](std::uint64_t extent) {
    surface_geometry = extent;
    int width = extent >> 32, height = unsigned(extent);
    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    auto state = get_state(r);
    while ((state.safe.w != width || state.safe.h != height) &&
           std::chrono::steady_clock::now() < deadline) {
      usleep(10000);
      state = get_state(r);
    }
    return state;
  };
  for (auto extent : {geometry(720, 400), geometry(400, 720), geometry(500, 500),
                      geometry(360, 500), geometry(800, 360)}) {
    auto state = wait_for_extent(extent);
    CHECK(state.error.empty() && state.safe.w == int(extent >> 32) &&
              state.safe.h == unsigned(extent),
          "Paused picture stayed stretched after a delayed window resize");
    CHECK(state.paused && state.time == 123. && state.yaw == .7f && state.zoom == 1.2f,
          "Window repair changed the paused scene");
    auto add = layout_controls(state.safe).add;
    CHECK(gpu::contains(add, visible_add_x, visible_add_y),
          "Delayed resize left the controls at the previous geometry");
    usleep(250000);
    CHECK(get_state(r).frames == state.frames, "Paused scene rendered continuously after repair");
  }
  float x = visible_add_x, y = visible_add_y;
  touch(r, Touch::kDown, x, y);
  touch(r, Touch::kUp, x, y);
  auto snapshot = capture_state(r);
  CHECK(snapshot && snapshot->count == 42, "Repaired control missed its touch target");

  // Temporarily zero-sized surfaces must recover without a fresh callback.
  CHECK(redraw(r), "Cannot settle input before surface loss");
  auto frames = get_state(r).frames;
  surface_geometry = geometry(1, 0);
  usleep(250000);
  CHECK(get_state(r).frames == frames && get_state(r).error.empty(),
        "Temporary zero extent was rendered or treated as fatal");
  auto recovered = wait_for_extent(geometry(400, 720));
  CHECK(recovered.safe.w == 400 && recovered.safe.h == 720 && recovered.time == 123.,
        "Zero-sized paused surface did not recover");

  // Activity pause does not hide a split-screen window. Geometry monitoring
  // continues there; onStop disables it, and onStart repairs the saved view.
  set_resumed(r, false);
  CHECK(redraw(r), "Cannot pause the visible Activity");
  auto visible = wait_for_extent(geometry(720, 400));
  CHECK(visible.safe.w == 720 && visible.safe.h == 400,
        "Visible paused Activity ignored a delayed resize");
  set_visible(r, false);
  CHECK(capture_state(r), "Cannot hide the Activity");
  frames = get_state(r).frames;
  unsigned checks = surface_checks;
  surface_geometry = geometry(500, 500);
  usleep(250000);
  CHECK(surface_checks == checks && get_state(r).frames == frames,
        "Hidden Activity kept monitoring or drawing its window");
  set_visible(r, true);
  auto shown = wait_for_extent(geometry(500, 500));
  CHECK(shown.safe.w == 500 && shown.safe.h == 500 && shown.time == 123.,
        "Showing the Activity kept hidden-window geometry");
  CHECK(detach_surface(r) && !renderer_alive, "Cannot detach repaired surface");
  checks = surface_checks;
  usleep(250000);
  CHECK(surface_checks == checks, "Geometry check touched a detached surface");
  CHECK(set_surface(r, nullptr, 400, 720) && redraw(r), "Cannot recreate paused surface");
  auto recreated = wait_for_extent(geometry(720, 400));
  CHECK(recreated.safe.w == 720 && recreated.safe.h == 400,
        "Recreated surface lost idle resize monitoring");

  surface_query_failure = true;
  auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
  while (get_state(r).error.empty() && std::chrono::steady_clock::now() < deadline) usleep(10000);
  CHECK(get_state(r).error == "Surface query failed", "Idle surface query failure was hidden");
  CHECK(!detach_surface(r) && !renderer_alive, "Query failure prevented surface release");
  surface_query_failure = false;
  surface_geometry = 0;
  return {};
}

// Ten seconds of vsync animate on an exact grid of the display's refreshes. A fast
// GPU draws every refresh, including while Android runs a 120 Hz display at 60 Hz
// for two seconds. A GPU too slow for 120 Hz settles on every other refresh, at
// 60 fps, and returns to 120 fps once its frames fit again. A slower GPU settles
// on every third refresh, at 40 fps, even with Choreographer calling back only on
// every other vsync. A GPU that starts slowly and slows down at 60 fps still
// finds and keeps 120 fps, occasional costly frames at 60 fps start earlier
// instead of lowering the rate, and a GPU time that settles from a slow start
// keeps 120 fps throughout.
Result<void> check_animation_clock(const char* directory) {
  struct Run {
    std::vector<Mode> modes;
    double start;
  };

  constexpr int64_t kSlow = 12000000, kFast = 5000000, kSlower = 24000000, kFaster = 7000000,
                    kUneven = 13000000, kSettling = 7500000;
  for (const auto& [modes, start] :
       {Run{{{600, 60, 0}}, 65536.}, Run{{{600, 60, 0}}, 262144.}, Run{{{600, 60, 0}}, 524288.},
        Run{{{1200, 120, 0}}, 65536.}, Run{{{480, 120, 0}, {120, 60, 0}, {480, 120, 0}}, 65536.},
        Run{{{1200, 120, kSlow}}, 65536.}, Run{{{360, 120, kSlow}, {840, 120, kFast}}, 65536.},
        Run{{{1200, 120, kSlower, 2}}, 65536.}, Run{{{1200, 120, kFaster, 1, 40, 1.6f}}, 65536.},
        Run{{{1200, 120, kUneven, 1, 0, 1, 20, 19000000}}, 65536.},
        Run{{{1200, 120, kSettling, 1, 0, 1, 0, 0, 2}}, 65536.}}) {
    unsigned callbacks = 0;
    bool fast = true;
    for (auto mode : modes) {
      callbacks += mode.callbacks;
      fast = fast && !mode.cost;
    }
    timespec now;
    __real_clock_gettime(CLOCK_MONOTONIC, &now);
    common::Owner<FrameClock> clock(new FrameClock{eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK), modes});
    clock->remaining = callbacks;
    clock->presentation = simulated_now = int64_t(now.tv_sec + 1) * 1000000000 + now.tv_nsec;
    CHECK(clock->fd >= 0, "Cannot create the test frame clock");
    frame_clock = clock.get();
    frame_cost = 0;
    warmup_frames = modes.front().warmup;
    settle_excess = modes.front().settle;
    presents = 0;
    present_time = present_spacing = 0;
    late_frames = uneven_presents = 0;
    {
      auto runtime = create_runtime(directory, SessionState{.count = 0, .time = start});
      CHECK(runtime, "Cannot restore the long-running animation");
      auto& r = **runtime;
      CHECK(set_surface(r, nullptr, 400, 720) && redraw(r), "Cannot attach clock test scene");
      auto frames = get_state(r).frames;
      set_resumed(r, true);
      auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(20);
      while (!clock->finished && std::chrono::steady_clock::now() < deadline) usleep(1000);
      CHECK(clock->finished, "Synthetic animation frames did not finish");
      unsigned drawn = get_state(r).frames - frames;
      if (fast) {
        // Every vsync draws a frame, after a pipeline latency of up to three
        // refreshes. Only display mode changes may change the spacing of
        // requested times, and pacing may skip a refresh as it re-anchors.
        CHECK(drawn + 3 >= callbacks && drawn <= callbacks &&
                  uneven_presents <= 2 * (modes.size() - 1) &&
                  present_spacing == 1000000000 / modes.back().rate,
              "Animation did not draw every vsync evenly");
      } else if (modes.back().cost == kSlow) {
        CHECK(drawn > 600 && drawn < 630 && present_spacing == 1000000000 / 60,
              "A GPU too slow for 120 Hz did not settle at 60 fps");
      } else if (modes.back().cost == kSlower) {
        CHECK(drawn > 400 && drawn < 440 && present_spacing == 3 * (1000000000 / 120),
              "A GPU too slow for 60 Hz did not settle at 40 fps");
      } else if (modes.back().cost == kSettling) {
        CHECK(drawn + 3 >= callbacks && present_spacing == 1000000000 / 120,
              "A GPU time settling from its start left 120 fps");
      } else if (modes.back().cost == kUneven) {
        CHECK(drawn > 590 && drawn < 640 && present_spacing == 1000000000 / 60,
              "Occasional costly frames lowered the rate below 60 fps");
      } else if (modes.back().cost == kFaster) {
        CHECK(drawn > 1100 && present_spacing == 1000000000 / 120,
              "A GPU slowed by warm-up and lower clocks did not keep 120 fps");
      } else {
        CHECK(drawn > 900 && drawn < 1100 && present_spacing == 1000000000 / 120,
              "A GPU that became fast enough did not return to 120 fps");
      }
      activate(r, Control::kPause);
      auto state = capture_state(r);
      CHECK(state && state->paused && std::abs(state->time - start - 10.) < .07,
            "Long-running animation drifted or froze");
      auto restored = decode_state(encode_state(*state));
      CHECK(restored && restored->time == state->time,
            "Saving animation time lost sub-frame precision");
      CHECK(redraw(r) && get_state(r).time == state->time,
            "Paused animation advanced during redraw");
    }
    frame_clock = nullptr;
    frame_cost = 0;
  }
  return {};
}

Result<void> check_batched_touch(const char* directory) {
  auto runtime = create_runtime(directory, SessionState{.count = 41, .paused = true});
  CHECK(runtime, "Cannot create batched-touch runtime");
  auto& r = **runtime;
  CHECK(set_surface(r, nullptr, 400, 720) && redraw(r), "Cannot attach batched-touch scene");
  set_touch_slop(r, 8);
  auto gestures = create_gestures(r);
  CHECK(gestures, "Cannot create batched-touch gestures");
  for (auto excursion : {gpu::Vec3{340, 650, 0}, gpu::Vec3{310, 680, 0}}) {
    AInputEvent down{AMOTION_EVENT_ACTION_DOWN, {310, 650, 0}, {}};
    AInputEvent move{AMOTION_EVENT_ACTION_MOVE, {310, 650, 0}, {excursion}};
    AInputEvent up{AMOTION_EVENT_ACTION_UP, {310, 650, 0}, {}};
    handle_motion(**gestures, down);
    handle_motion(**gestures, move);
    handle_motion(**gestures, up);
    auto state = capture_state(r);
    CHECK(state && state->count == 41, "Batched movement activated Reset after leaving slop");
  }
  AInputEvent down{AMOTION_EVENT_ACTION_DOWN, {310, 650, 0}, {}};
  AInputEvent move{AMOTION_EVENT_ACTION_MOVE, {310, 650, 0}, {{311, 651, 0}, {309, 649, 0}}};
  AInputEvent up{AMOTION_EVENT_ACTION_UP, {310, 650, 0}, {}};
  handle_motion(**gestures, down);
  handle_motion(**gestures, move);
  handle_motion(**gestures, up);
  auto state = capture_state(r);
  CHECK(state && state->count == 0, "Batched sensor jitter cancelled a valid Reset tap");
  return {};
}

Result<void> check_stalled_worker(const char* directory, Stall stall) {
  auto begin = std::chrono::steady_clock::now();
  pid_t child = fork();
  CHECK(child >= 0, "Cannot fork the stalled-worker test");
  if (!child) {
    const rlimit limit{0, 0};
    setrlimit(RLIMIT_CORE, &limit);
    // These aborts are intentional. Measure the worker deadline without
    // Android's crash reporter adding its dump latency before process exit.
    std::signal(SIGABRT, SIG_DFL);
    auto runtime = create_runtime(directory);
    if (!runtime) _exit(1);
    auto& r = **runtime;
    if (!set_surface(r, nullptr, 400, 720) || !redraw(r)) _exit(2);
    if (stall == Stall::kDestroy) {
      stall_destroy = true;
      runtime->reset();
      _exit(3);
    }
    stall_render = true;
    set_content(r, {});
    for (int i = 0; i < 1000 && !worker_stalled; ++i) usleep(1000);
    if (!worker_stalled) _exit(4);
    if (stall == Stall::kSnapshot) {
      auto snapshot = capture_state(r);
      // Exit directly: the parent checks the snapshot's bound separately
      // from the fail-fast destructor cases below.
      _exit(!snapshot && snapshot.error().message.find("Timed out") != std::string::npos ? 42 : 5);
    }
    if (stall == Stall::kDetach) {
      (void)detach_surface(r);
      _exit(6);
    }
    if (redraw(r)) _exit(7);
    runtime->reset();
    _exit(8);
  }
  int status = 0;
  for (;;) {
    if (waitpid(child, &status, WNOHANG) == child) break;
    if (std::chrono::duration<double>(std::chrono::steady_clock::now() - begin).count() > 6) {
      kill(child, SIGKILL);
      waitpid(child, &status, 0);
      return std::unexpected(Error{"A lifecycle operation left the main thread blocked"});
    }
    usleep(10000);
  }
  double elapsed = std::chrono::duration<double>(std::chrono::steady_clock::now() - begin).count();
  std::fprintf(stderr, "Lifecycle stall %d completed in %.3f seconds\n", int(stall), elapsed);
  if (stall == Stall::kSnapshot)
    CHECK(WIFEXITED(status) && WEXITSTATUS(status) == 42, "Snapshot did not return a timeout");
  else
    CHECK(WIFSIGNALED(status) && WTERMSIG(status) == SIGABRT,
          "Unsafe surface/worker cleanup did not terminate");
  CHECK(elapsed >= 2.5 && elapsed < 5, "Lifecycle cleanup exceeded its total deadline");
  return {};
}

Result<void> check_faults(const char* directory) {
  auto runtime = create_runtime(directory);
  CHECK(runtime, "Cannot create runtime");
  auto& r = **runtime;
  activate(r, Control::kPause);
  CHECK(set_surface(r, nullptr, 400, 720) && redraw(r), "Cannot initialize the paused scene");
  auto initial = get_state(r);
  CHECK(initial.paused && initial.safe.w == 400 && initial.safe.h == 720, "Wrong initial layout");

  for (auto* deferrals : {&prepare_deferrals, &present_deferrals}) {
    unsigned previous_waits = waits, previous_frames = get_state(r).frames;
    *deferrals = 4;
    CHECK(redraw(r), "Transient deferral did not recover");
    CHECK(*deferrals == 0 && waits == previous_waits + 1 &&
              get_state(r).frames == previous_frames + 1,
          "Deferred redraw was acknowledged without waiting for the completed frame");
  }

  resize_during_render = true;
  CHECK(redraw(r), "Resize during frame layout did not recover");
  auto resized = get_state(r);
  CHECK(resized.safe.w == 720 && resized.safe.h == 320 && resized.time == initial.time,
        "Paused resize published stale dimensions or advanced time");
  float x = visible_add_x, y = visible_add_y;
  auto expected = layout_controls(resized.safe).add;
  CHECK(gpu::contains(expected, x, y), "Published layout disagrees with the visible button");
  touch(r, Touch::kDown, x, y);
  touch(r, Touch::kUp, x, y);
  CHECK(redraw(r) && get_state(r).count == initial.count + 1,
        "The visible button missed its touch target after resizing");

  unavailable = true;
  unsigned previous_frames = get_state(r).frames;
  auto begin = std::chrono::steady_clock::now();
  auto result = redraw(r);
  double elapsed = std::chrono::duration<double>(std::chrono::steady_clock::now() - begin).count();
  CHECK(!result && result.error().message.find("Timed out") != std::string::npos,
        "Unavailable surface did not fail the required redraw");
  CHECK(elapsed >= 2.5 && elapsed < 4.5, "Required redraw did not respect its deadline");
  CHECK(!capture_state(r), "A snapshot hid the terminal rendering error");
  auto failed = get_state(r);
  CHECK(!failed.error.empty() && failed.frames == previous_frames && callbacks > 0,
        "Timeout did not stop retries without vsync");
  // The error remains visible, but detach must still release the surface.
  CHECK(!detach_surface(r) && !renderer_alive, "Timeout prevented surface cleanup");
  CHECK(!redraw(r), "A later redraw hid the terminal rendering error");
  return {};
}
}

int main() {
  std::string directory =
      std::string(std::getenv("TEST_TMPDIR") ? std::getenv("TEST_TMPDIR") : "/tmp") +
      "/native-buttons-fault-test.XXXXXX";
  if (!mkdtemp(directory.data())) return 1;
  Result<void> result;
  for (auto stall :
       {Stall::kSnapshot, Stall::kDetach, Stall::kDestroy, Stall::kRedrawThenDestroy}) {
    if (!result) break;
    result = check_stalled_worker(directory.c_str(), stall);
    if (!result) break;
  }
  if (result) result = check_idle_resize(directory.c_str());
  if (result) result = check_animation_clock(directory.c_str());
  if (result) result = check_batched_touch(directory.c_str());
  if (result) result = check_faults(directory.c_str());
  unlink((directory + "/count.txt").c_str());
  rmdir(directory.c_str());
  if (!result) {
    std::fprintf(stderr, "%s\n", result.error().message.c_str());
    return 1;
  }
  std::puts(
      "Idle/hidden window resizing, deferred redraw recovery/deadline, missing vsync, resize "
      "and hit targets passed");
  return 0;
}
