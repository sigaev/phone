#include "native_buttons/runtime.h"

#include <android/choreographer.h>
#include <android/log.h>
#include <android/looper.h>
#include <android/native_window.h>
#include <dlfcn.h>
#include <pthread.h>
#include <sys/eventfd.h>
#include <time.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <new>
#include <utility>
#include <vector>

#include "common/gpu/renderer.h"
#include "native_buttons/scene.h"
#include "native_buttons/storage.h"

namespace native_buttons {
using common::Error;
using common::Owner;
using common::Result;
namespace {
constexpr int kLifecycleTimeoutSeconds = 3;
constexpr char kRedrawTimeoutError[] = "Timed out waiting for the window to redraw";
constexpr char kSnapshotTimeoutError[] = "Timed out waiting for the Activity state";
[[noreturn]] void stop_unresponsive_worker(const char* operation) {
    __android_log_print(ANDROID_LOG_FATAL, "native_buttons",
                        "Rendering worker did not %s before the lifecycle deadline", operation);
    std::abort();
}
struct Window {
    ANativeWindow* handle;
};
void destroy(Window* window) noexcept {
    ANativeWindow_release(window->handle);
    delete window;
}
enum class CommandKind {
    kSurface,
    kDetach,
    kRedraw,
    kLifecycleTimeout,
    kSnapshot,
    kResume,
    kContent,
    kActivate,
    kFocus,
    kKey,
    kTouch,
    kPinch,
    kTouchSlop,
    kDensity,
    kStop
};
struct Command {
    CommandKind kind;
    unsigned sequence = 0;
    Owner<Window> window;
    gpu::Rect rect;
    int value = 0;
    float x = 0, y = 0;
};
void signal_fd(int fd) {
    const uint64_t one = 1;
    while (write(fd, &one, sizeof(one)) < 0 && errno == EINTR) {
    }
}
void drain_fd(int fd) {
    uint64_t value;
    while (read(fd, &value, sizeof(value)) < 0 && errno == EINTR) {
    }
}
}  // namespace

struct Runtime {
    pthread_mutex_t mutex = PTHREAD_MUTEX_INITIALIZER;
    pthread_cond_t completed_condition;
    pthread_t thread{};
    bool started = false, condition_initialized = false;
    int commands_fd = -1, notifications_fd = -1;
    std::vector<Command> commands;
    unsigned next_sequence = 0, completed_sequence = 0;
    unsigned completed_redraw_sequence = 0;
    bool exited = false;
    bool deadline_failed = false;
    timespec recovery_deadline{};
    RuntimeState published;

    // Everything below is confined to the rendering thread.
    RuntimeState state;
    std::string directory;
    int restored = -1;
    Owner<Window> window;
    Owner<gpu::Renderer> renderer;
    Owner<Scene> scene;
    AChoreographer* choreographer = nullptr;
    gpu::Rect content;
    bool resumed = false, stopping = false, frame_pending = false, dirty_count = false;
    bool dirty_visual = false;
    unsigned redraw_sequence = 0;
    bool dragging = false;
    float last_x = 0, down_x = 0, down_y = 0, touch_slop = 8, density = 1, fps = 0;
    long last_frame = 0;
    double fps_time = 0;
    unsigned fps_frames = 0;
};

namespace {
// Called with the mutex held. A required window redraw is acknowledged only
// after drawing completes, including retries for transient surface conditions.
void publish_state(Runtime& r) {
    r.published = r.state;
    if (r.redraw_sequence && (!r.dirty_visual || !r.scene || !r.state.error.empty())) {
        r.completed_redraw_sequence = r.redraw_sequence;
        r.redraw_sequence = 0;
    }
    pthread_cond_broadcast(&r.completed_condition);
}
void publish(Runtime& r) {
    pthread_mutex_lock(&r.mutex);
    publish_state(r);
    pthread_mutex_unlock(&r.mutex);
    signal_fd(r.notifications_fd);
}
void reset_timing(Runtime& r) {
    r.last_frame = 0;
    r.fps = 0;
    r.fps_time = 0;
    r.fps_frames = 0;
}
void persist(Runtime& r) {
    if (!r.dirty_count)
        return;
    auto result = save_count(r.directory.c_str(), r.state.count);
    r.state.saved = bool(result);
    r.state.save_error = result ? "" : result.error().message;
    if (result)
        r.dirty_count = false;
}
void apply_control(Runtime& r, Control control) {
    switch (control) {
        case Control::kAdd:
            r.state.count = std::min(999999, r.state.count + 1);
            r.dirty_count = true;
            break;
        case Control::kReset:
            r.state.count = 0;
            r.dirty_count = true;
            break;
        case Control::kQuality:
            r.state.maximum = !r.state.maximum;
            break;
        case Control::kPause:
            r.state.paused = !r.state.paused;
            reset_timing(r);
            break;
        default:
            break;
    }
    persist(r);
}
void apply_key(Runtime& r, Key action) {
    constexpr Control order[] = {Control::kQuality, Control::kPause, Control::kAdd,
                                 Control::kReset};
    if (action == Key::kActivate) {
        apply_control(r, r.state.focused);
        return;
    }
    int index = -1;
    for (int i = 0; i < 4; ++i)
        if (order[i] == r.state.focused)
            index = i;
    bool reverse = action == Key::kPrevious;
    int target = index < 0 ? (reverse ? 3 : 0) : (index + (reverse ? 3 : 1)) % 4;
    r.state.focused = order[target];
}
void apply_touch(Runtime& r, const Command& command) {
    float x = command.x, y = command.y;
    Control hit = r.scene ? static_cast<Control>(hit_test(*r.scene, x, y)) : Control::kNone;
    float dx = x - r.down_x, dy = y - r.down_y;
    bool moved = dx * dx + dy * dy > r.touch_slop * r.touch_slop;
    switch (static_cast<Touch>(command.value)) {
        case Touch::kDown:
            r.state.focused = Control::kNone;
            r.state.pressed = hit;
            r.dragging = hit == Control::kNone;
            r.down_x = x;
            r.down_y = y;
            break;
        case Touch::kMove: {
            float dx = x - r.last_x;
            if (r.dragging && r.renderer) {
                int width = gpu::get_stats(*r.renderer).width;
                r.state.yaw =
                    std::remainder(r.state.yaw - dx / std::max(1, width) * gpu::kPi, 2 * gpu::kPi);
            }
            if (moved || hit != r.state.pressed)
                r.state.pressed = Control::kNone;
            break;
        }
        case Touch::kUp:
            // Check the release too, even when Android did not send a MOVE.
            if (r.state.pressed != Control::kNone && hit == r.state.pressed && !moved)
                apply_control(r, hit);
            [[fallthrough]];
        case Touch::kCancel:
            r.state.pressed = Control::kNone;
            r.dragging = false;
            break;
    }
    r.last_x = x;
}
Result<void> draw(Runtime& r, bool wait) {
    if (!r.scene || !r.state.error.empty())
        return {};
    r.dirty_visual = true;
    Control highlighted = r.state.pressed == Control::kNone ? r.state.focused : r.state.pressed;
    auto result = render_scene(*r.scene, r.state.time, r.state.yaw, r.state.maximum, r.state.count,
                               static_cast<int>(highlighted), r.fps, r.state.paused, r.content,
                               r.state.saved, true, r.density, r.state.zoom);
    if (!result)
        return std::unexpected(result.error());
    if (!*result)
        return {};
    auto stats = gpu::get_stats(*r.renderer);
    r.state.safe = safe_area(r.content, stats.width, stats.height);
    if (wait || r.redraw_sequence) {
        if (auto waited = gpu::wait_frame(*r.renderer); !waited)
            return waited;
    }
    r.dirty_visual = false;
    ++r.state.frames;
    return {};
}
void record_result(Runtime& r, const Result<void>& result) {
    if (!result)
        r.state.error = result.error().message;
}
bool needs_frame(const Runtime& r) {
    return !r.stopping && r.scene && r.state.error.empty() &&
           (r.dirty_visual || (r.resumed && !r.state.paused));
}
void schedule_frame(Runtime& r);
void on_frame(long nanos, void* data) {
    auto& r = *static_cast<Runtime*>(data);
    r.frame_pending = false;
    if (!needs_frame(r))
        return;
    bool animating = r.resumed && !r.state.paused;
    if (animating) {
        double delta = r.last_frame ? double(nanos - r.last_frame) / 1e9 : 1. / 60;
        r.last_frame = nanos;
        r.state.time += std::clamp(delta, 0., .1);
        r.fps_time += delta;
    } else {
        reset_timing(r);
    }
    unsigned previous_frames = r.state.frames;
    record_result(r, draw(r, false));
    if (animating) {
        r.fps_frames += r.state.frames != previous_frames;
        if (r.fps_time > .5) {
            r.fps = r.fps_frames / r.fps_time;
            r.fps_frames = 0;
            r.fps_time = 0;
        }
    }
    publish(r);
    schedule_frame(r);
}
void schedule_frame(Runtime& r) {
    if (needs_frame(r) && !r.frame_pending) {
        r.frame_pending = true;
        AChoreographer_postFrameCallback(r.choreographer, on_frame, &r);
    }
}
void release_surface(Runtime& r) {
    r.scene.reset();
    r.renderer.reset();
    r.window.reset();
    reset_timing(r);
    r.dirty_visual = false;
    r.state.pressed = Control::kNone;
    r.dragging = false;
    r.state.safe = {};
}
Result<void> create_surface(Runtime& r, Command& command) {
    release_surface(r);
    r.window = std::move(command.window);
    auto renderer = gpu::create_renderer(r.window ? r.window->handle : nullptr, get_scene_shaders(),
                                         int(command.rect.w), int(command.rect.h));
    if (!renderer)
        return std::unexpected(renderer.error());
    r.renderer = std::move(*renderer);
    auto scene = create_scene(*r.renderer);
    if (!scene)
        return std::unexpected(scene.error());
    r.scene = std::move(*scene);
    if (r.window) {
        using SetRate = int (*)(ANativeWindow*, float, int8_t);
        auto set_rate =
            reinterpret_cast<SetRate>(dlsym(RTLD_DEFAULT, "ANativeWindow_setFrameRate"));
        if (set_rate)
            set_rate(r.window->handle, 60.f, 0);
    }
    return draw(r, false);
}
int process_commands(int, int, void* data) {
    auto& r = *static_cast<Runtime*>(data);
    drain_fd(r.commands_fd);
    std::vector<Command> commands;
    pthread_mutex_lock(&r.mutex);
    commands.swap(r.commands);
    pthread_mutex_unlock(&r.mutex);
    for (auto& command : commands) {
        switch (command.kind) {
            case CommandKind::kSurface:
                if (r.state.error.empty())
                    record_result(r, create_surface(r, command));
                command.window.reset();
                break;
            case CommandKind::kDetach:
                release_surface(r);
                break;
            case CommandKind::kRedraw:
                r.redraw_sequence = command.sequence;
                record_result(r, draw(r, true));
                break;
            case CommandKind::kLifecycleTimeout:
                r.state.error = command.value ? kRedrawTimeoutError : kSnapshotTimeoutError;
                break;
            case CommandKind::kSnapshot:
                // A lifecycle snapshot must include queued input and startup loading.
                break;
            case CommandKind::kResume:
                r.resumed = command.value;
                reset_timing(r);
                r.state.pressed = Control::kNone;
                r.dragging = false;
                if (!r.resumed) {
                    persist(r);
                }
                r.dirty_visual = true;
                break;
            case CommandKind::kContent:
                r.content = command.rect;
                r.state.pressed = Control::kNone;
                r.dragging = false;
                r.dirty_visual = true;
                break;
            case CommandKind::kActivate:
                apply_control(r, static_cast<Control>(command.value));
                r.dirty_visual = true;
                break;
            case CommandKind::kFocus:
                r.state.focused = static_cast<Control>(command.value);
                r.dirty_visual = true;
                break;
            case CommandKind::kKey:
                apply_key(r, static_cast<Key>(command.value));
                r.dirty_visual = true;
                break;
            case CommandKind::kTouch:
                apply_touch(r, command);
                r.dirty_visual = true;
                break;
            case CommandKind::kPinch:
                r.state.pressed = Control::kNone;
                r.dragging = false;
                r.state.zoom = std::clamp(r.state.zoom * command.x, kMinimumZoom, kMaximumZoom);
                r.dirty_visual = true;
                break;
            case CommandKind::kTouchSlop:
                r.touch_slop = command.x;
                r.state.pressed = Control::kNone;
                r.dragging = false;
                r.dirty_visual = true;
                break;
            case CommandKind::kDensity:
                r.density = command.x;
                r.state.pressed = Control::kNone;
                r.dragging = false;
                r.dirty_visual = true;
                break;
            case CommandKind::kStop:
                persist(r);
                release_surface(r);
                r.stopping = true;
                break;
        }
        pthread_mutex_lock(&r.mutex);
        r.completed_sequence = command.sequence;
        publish_state(r);
        pthread_mutex_unlock(&r.mutex);
    }
    if (r.dirty_visual && (!r.resumed || r.state.paused) && !r.stopping)
        record_result(r, draw(r, false));
    publish(r);
    schedule_frame(r);
    return 1;
}
void* worker(void* data) {
    auto& r = *static_cast<Runtime*>(data);
    pthread_setname_np(pthread_self(), "pelican-render");
    auto count = load_count(r.directory.c_str());
    r.state.count = r.restored >= 0 ? std::clamp(r.restored, 0, 999999) : count.value_or(0);
    if (!count) {
        r.state.saved = false;
        r.state.save_error = count.error().message;
    }
    if (r.restored >= 0 && (!count || *count != r.state.count)) {
        r.dirty_count = true;
        persist(r);
    }
    ALooper* looper = ALooper_prepare(0);
    r.choreographer = AChoreographer_getInstance();
    if (!looper || !r.choreographer ||
        ALooper_addFd(looper, r.commands_fd, ALOOPER_POLL_CALLBACK, ALOOPER_EVENT_INPUT,
                      process_commands, &r) != 1) {
        r.state.error = "Cannot initialize the rendering looper";
        r.stopping = true;
    }
    publish(r);
    while (!r.stopping)
        ALooper_pollOnce(-1, nullptr, nullptr, nullptr);
    if (looper)
        ALooper_removeFd(looper, r.commands_fd);
    pthread_mutex_lock(&r.mutex);
    r.exited = true;
    pthread_cond_broadcast(&r.completed_condition);
    // Last access to Runtime on this detached thread. Its owner may free it
    // after acquiring the mutex and observing exited, without joining TLS cleanup.
    pthread_mutex_unlock(&r.mutex);
    return nullptr;
}
unsigned enqueue(Runtime& r, Command command) {
    pthread_mutex_lock(&r.mutex);
    unsigned sequence = command.sequence = ++r.next_sequence;
    r.commands.push_back(std::move(command));
    pthread_mutex_unlock(&r.mutex);
    signal_fd(r.commands_fd);
    return sequence;
}
Result<void> synchronize(Runtime& r, CommandKind kind) {
    timespec deadline{};
    clock_gettime(CLOCK_MONOTONIC, &deadline);
    deadline.tv_sec += kLifecycleTimeoutSeconds;
    unsigned sequence = enqueue(r, Command{kind});
    pthread_mutex_lock(&r.mutex);
    bool releasing = kind == CommandKind::kDetach || kind == CommandKind::kStop;
    if (r.deadline_failed)
        deadline = r.recovery_deadline;
    auto pending = [&] {
        return !r.exited &&
               (kind == CommandKind::kStop || r.completed_sequence < sequence ||
                (kind == CommandKind::kRedraw && r.completed_redraw_sequence < sequence));
    };
    while (pending()) {
        int result = pthread_cond_timedwait(&r.completed_condition, &r.mutex, &deadline);
        if (result && pending()) {
            if (!r.deadline_failed) {
                r.deadline_failed = true;
                clock_gettime(CLOCK_MONOTONIC, &r.recovery_deadline);
                ++r.recovery_deadline.tv_sec;
            }
            pthread_mutex_unlock(&r.mutex);
            // Returning from detach with live GPU/window users is unsafe. Stop
            // also owns all pending callback data, so neither may abandon the worker.
            if (releasing)
                stop_unresponsive_worker(kind == CommandKind::kStop ? "stop"
                                                                    : "release its window");
            Command timeout{CommandKind::kLifecycleTimeout};
            timeout.value = kind == CommandKind::kRedraw;
            enqueue(r, std::move(timeout));
            return std::unexpected(
                Error{kind == CommandKind::kRedraw ? kRedrawTimeoutError : kSnapshotTimeoutError});
        }
    }
    std::string error = r.published.error;
    pthread_mutex_unlock(&r.mutex);
    if (!error.empty())
        return std::unexpected(Error{std::move(error)});
    return {};
}
}  // namespace

Result<Owner<Runtime>> create_runtime(const char* directory, SessionState restored) {
    if (restored.count < -1 || restored.count > 999999 || !std::isfinite(restored.yaw) ||
        !std::isfinite(restored.time) || restored.time < 0 || !std::isfinite(restored.zoom) ||
        restored.zoom < kMinimumZoom || restored.zoom > kMaximumZoom)
        return std::unexpected(Error{"Invalid restored session state"});
    Owner<Runtime> runtime(new (std::nothrow) Runtime);
    if (!runtime)
        return std::unexpected(Error{"Cannot allocate application runtime"});
    pthread_condattr_t attributes;
    if (pthread_condattr_init(&attributes) != 0)
        return std::unexpected(Error{"Cannot initialize runtime synchronization"});
    int error = pthread_condattr_setclock(&attributes, CLOCK_MONOTONIC);
    if (!error)
        error = pthread_cond_init(&runtime->completed_condition, &attributes);
    pthread_condattr_destroy(&attributes);
    if (error)
        return std::unexpected(Error{"Cannot initialize runtime synchronization"});
    runtime->condition_initialized = true;
    runtime->directory = directory;
    runtime->restored = restored.count;
    static_cast<SessionState&>(runtime->state) = restored;
    runtime->state.count = std::max(0, restored.count);
    runtime->published = runtime->state;
    runtime->commands_fd = eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK);
    runtime->notifications_fd = eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK);
    if (runtime->commands_fd < 0 || runtime->notifications_fd < 0)
        return std::unexpected(Error{"Cannot create runtime notification channels"});
    pthread_attr_t thread_attributes;
    if (pthread_attr_init(&thread_attributes) != 0)
        return std::unexpected(Error{"Cannot initialize rendering thread attributes"});
    error = pthread_attr_setdetachstate(&thread_attributes, PTHREAD_CREATE_DETACHED);
    if (!error)
        error = pthread_create(&runtime->thread, &thread_attributes, worker, runtime.get());
    pthread_attr_destroy(&thread_attributes);
    if (error)
        return std::unexpected(Error{"Cannot start the rendering thread"});
    runtime->started = true;
    return runtime;
}
void destroy(Runtime* runtime) noexcept {
    if (!runtime)
        return;
    if (runtime->started) {
        (void)synchronize(*runtime, CommandKind::kStop);
    }
    if (runtime->commands_fd >= 0)
        close(runtime->commands_fd);
    if (runtime->notifications_fd >= 0)
        close(runtime->notifications_fd);
    if (runtime->condition_initialized)
        pthread_cond_destroy(&runtime->completed_condition);
    pthread_mutex_destroy(&runtime->mutex);
    delete runtime;
}
Result<void> set_surface(Runtime& r, ANativeWindow* window, int width, int height) {
    Command command{CommandKind::kSurface};
    if (window) {
        command.window.reset(new (std::nothrow) Window{window});
        if (!command.window)
            return std::unexpected(Error{"Cannot allocate window state"});
        ANativeWindow_acquire(window);
    } else if (width <= 0 || height <= 0) {
        return std::unexpected(Error{"Offscreen dimensions must be positive"});
    }
    command.rect = {0, 0, float(width), float(height)};
    enqueue(r, std::move(command));
    return {};
}
Result<void> detach_surface(Runtime& r) {
    return synchronize(r, CommandKind::kDetach);
}
Result<void> redraw(Runtime& r) {
    return synchronize(r, CommandKind::kRedraw);
}
void set_resumed(Runtime& r, bool resumed) {
    Command command{CommandKind::kResume};
    command.value = resumed;
    enqueue(r, std::move(command));
}
void set_content(Runtime& r, gpu::Rect content) {
    Command command{CommandKind::kContent};
    command.rect = content;
    enqueue(r, std::move(command));
}
void activate(Runtime& r, Control control) {
    Command command{CommandKind::kActivate};
    command.value = static_cast<int>(control);
    enqueue(r, std::move(command));
}
void touch(Runtime& r, Touch action, float x, float y) {
    Command command{CommandKind::kTouch};
    command.value = static_cast<int>(action);
    command.x = x;
    command.y = y;
    enqueue(r, std::move(command));
}
void set_touch_slop(Runtime& r, float pixels) {
    if (!std::isfinite(pixels) || pixels <= 0)
        return;
    Command command{CommandKind::kTouchSlop};
    command.x = pixels;
    enqueue(r, std::move(command));
}
void pinch(Runtime& r, float scale) {
    if (!std::isfinite(scale) || scale <= 0)
        return;
    Command command{CommandKind::kPinch};
    command.x = scale;
    enqueue(r, std::move(command));
}
void set_density(Runtime& r, float pixels_per_dp) {
    if (!std::isfinite(pixels_per_dp) || pixels_per_dp <= 0)
        return;
    Command command{CommandKind::kDensity};
    command.x = pixels_per_dp;
    enqueue(r, std::move(command));
}
void focus_control(Runtime& r, Control control) {
    Command command{CommandKind::kFocus};
    command.value = static_cast<int>(control);
    enqueue(r, std::move(command));
}
void key(Runtime& r, Key action) {
    Command command{CommandKind::kKey};
    command.value = static_cast<int>(action);
    enqueue(r, std::move(command));
}
RuntimeState get_state(Runtime& r) {
    pthread_mutex_lock(&r.mutex);
    RuntimeState state = r.published;
    pthread_mutex_unlock(&r.mutex);
    return state;
}
Result<RuntimeState> capture_state(Runtime& r) {
    if (auto result = synchronize(r, CommandKind::kSnapshot); !result)
        return std::unexpected(result.error());
    return get_state(r);
}
int notification_fd(const Runtime& r) {
    return r.notifications_fd;
}
void acknowledge_notifications(Runtime& r) {
    drain_fd(r.notifications_fd);
}
}  // namespace native_buttons
