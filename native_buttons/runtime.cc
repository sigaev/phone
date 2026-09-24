#include "native_buttons/runtime.h"

#include <android/choreographer.h>
#include <android/looper.h>
#include <android/native_window.h>
#include <dlfcn.h>
#include <pthread.h>
#include <sys/eventfd.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <cmath>
#include <cstdint>
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
    kSnapshot,
    kResume,
    kContent,
    kActivate,
    kFocus,
    kKey,
    kTouch,
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
    pthread_cond_t completed_condition = PTHREAD_COND_INITIALIZER;
    pthread_t thread{};
    bool started = false;
    int commands_fd = -1, notifications_fd = -1;
    std::vector<Command> commands;
    unsigned next_sequence = 0, completed_sequence = 0;
    bool exited = false;
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
    bool dragging = false;
    float last_x = 0, last_y = 0, travel = 0, fps = 0;
    long last_frame = 0;
    double fps_time = 0;
    unsigned fps_frames = 0;
};

namespace {
void publish(Runtime& r) {
    pthread_mutex_lock(&r.mutex);
    r.published = r.state;
    pthread_mutex_unlock(&r.mutex);
    signal_fd(r.notifications_fd);
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
    Control hit = hit_test(layout_controls(r.state.safe), x, y);
    switch (static_cast<Touch>(command.value)) {
        case Touch::kDown:
            r.state.focused = Control::kNone;
            r.state.pressed = hit;
            r.dragging = hit == Control::kNone;
            r.travel = 0;
            break;
        case Touch::kMove: {
            float dx = x - r.last_x, dy = y - r.last_y;
            r.travel += std::fabs(dx) + std::fabs(dy);
            if (r.dragging && r.renderer) {
                int width = gpu::get_stats(*r.renderer).width;
                r.state.yaw =
                    std::remainder(r.state.yaw - dx / std::max(1, width) * gpu::kPi, 2 * gpu::kPi);
            }
            if (hit != r.state.pressed)
                r.state.pressed = Control::kNone;
            break;
        }
        case Touch::kUp:
            // Include the final displacement even when Android did not send a MOVE.
            r.travel += std::fabs(x - r.last_x) + std::fabs(y - r.last_y);
            if (r.state.pressed != Control::kNone && hit == r.state.pressed && r.travel < 30)
                apply_control(r, hit);
            [[fallthrough]];
        case Touch::kCancel:
            r.state.pressed = Control::kNone;
            r.dragging = false;
            break;
    }
    r.last_x = x;
    r.last_y = y;
}
Result<void> draw(Runtime& r, bool wait) {
    if (!r.scene || !r.state.error.empty())
        return {};
    if (auto result = gpu::prepare_frame(*r.renderer, r.state.maximum); !result)
        return result;
    auto stats = gpu::get_stats(*r.renderer);
    r.state.safe = safe_area(r.content, stats.width, stats.height);
    Control highlighted = r.state.pressed == Control::kNone ? r.state.focused : r.state.pressed;
    auto result = render_scene(*r.scene, r.state.time, r.state.yaw, r.state.maximum, r.state.count,
                               static_cast<int>(highlighted), r.fps, r.state.paused, r.state.safe,
                               r.state.saved);
    if (!result)
        return result;
    if (wait) {
        if (auto waited = gpu::wait_frame(*r.renderer); !waited)
            return waited;
    }
    ++r.state.frames;
    return {};
}
void record_result(Runtime& r, const Result<void>& result) {
    if (!result)
        r.state.error = result.error().message;
}
void schedule_frame(Runtime& r);
void on_frame(long nanos, void* data) {
    auto& r = *static_cast<Runtime*>(data);
    r.frame_pending = false;
    if (r.stopping || !r.resumed || !r.scene || !r.state.error.empty())
        return;
    double delta = r.last_frame ? double(nanos - r.last_frame) / 1e9 : 1. / 60;
    r.last_frame = nanos;
    if (!r.state.paused)
        r.state.time += std::clamp(float(delta), 0.f, .1f);
    r.fps_time += delta;
    ++r.fps_frames;
    if (r.fps_time > .5) {
        r.fps = r.fps_frames / r.fps_time;
        r.fps_frames = 0;
        r.fps_time = 0;
    }
    record_result(r, draw(r, false));
    publish(r);
    schedule_frame(r);
}
void schedule_frame(Runtime& r) {
    if (r.resumed && r.scene && !r.stopping && r.state.error.empty() && !r.frame_pending) {
        r.frame_pending = true;
        AChoreographer_postFrameCallback(r.choreographer, on_frame, &r);
    }
}
void release_surface(Runtime& r) {
    r.scene.reset();
    r.renderer.reset();
    r.window.reset();
    r.last_frame = 0;
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
    bool dirty_visual = false;
    for (auto& command : commands) {
        switch (command.kind) {
            case CommandKind::kSurface:
                if (r.state.error.empty())
                    record_result(r, create_surface(r, command));
                command.window.reset();
                dirty_visual = false;
                break;
            case CommandKind::kDetach:
                release_surface(r);
                dirty_visual = false;
                break;
            case CommandKind::kRedraw:
                record_result(r, draw(r, true));
                dirty_visual = false;
                break;
            case CommandKind::kSnapshot:
                // A lifecycle snapshot must include queued input and startup loading.
                break;
            case CommandKind::kResume:
                r.resumed = command.value;
                r.last_frame = 0;
                r.fps_time = 0;
                r.fps_frames = 0;
                r.state.pressed = Control::kNone;
                r.dragging = false;
                if (!r.resumed) {
                    persist(r);
                    dirty_visual = true;
                }
                break;
            case CommandKind::kContent:
                r.content = command.rect;
                dirty_visual = true;
                break;
            case CommandKind::kActivate:
                apply_control(r, static_cast<Control>(command.value));
                dirty_visual = true;
                break;
            case CommandKind::kFocus:
                r.state.focused = static_cast<Control>(command.value);
                dirty_visual = true;
                break;
            case CommandKind::kKey:
                apply_key(r, static_cast<Key>(command.value));
                dirty_visual = true;
                break;
            case CommandKind::kTouch:
                apply_touch(r, command);
                dirty_visual = true;
                break;
            case CommandKind::kStop:
                persist(r);
                release_surface(r);
                r.stopping = true;
                break;
        }
        pthread_mutex_lock(&r.mutex);
        r.published = r.state;
        r.completed_sequence = command.sequence;
        pthread_cond_broadcast(&r.completed_condition);
        pthread_mutex_unlock(&r.mutex);
    }
    if (dirty_visual && !r.resumed && !r.stopping)
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
    unsigned sequence = enqueue(r, Command{kind});
    pthread_mutex_lock(&r.mutex);
    while (r.completed_sequence < sequence && !r.exited)
        pthread_cond_wait(&r.completed_condition, &r.mutex);
    std::string error = r.published.error;
    pthread_mutex_unlock(&r.mutex);
    if (!error.empty())
        return std::unexpected(Error{std::move(error)});
    return {};
}
}  // namespace

Result<Owner<Runtime>> create_runtime(const char* directory, int restored) {
    Owner<Runtime> runtime(new (std::nothrow) Runtime);
    if (!runtime)
        return std::unexpected(Error{"Cannot allocate application runtime"});
    runtime->directory = directory;
    runtime->restored = restored;
    runtime->commands_fd = eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK);
    runtime->notifications_fd = eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK);
    if (runtime->commands_fd < 0 || runtime->notifications_fd < 0)
        return std::unexpected(Error{"Cannot create runtime notification channels"});
    if (pthread_create(&runtime->thread, nullptr, worker, runtime.get()) != 0)
        return std::unexpected(Error{"Cannot start the rendering thread"});
    runtime->started = true;
    return runtime;
}
void destroy(Runtime* runtime) noexcept {
    if (!runtime)
        return;
    if (runtime->started) {
        enqueue(*runtime, Command{CommandKind::kStop});
        pthread_join(runtime->thread, nullptr);
    }
    if (runtime->commands_fd >= 0)
        close(runtime->commands_fd);
    if (runtime->notifications_fd >= 0)
        close(runtime->notifications_fd);
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
RuntimeState capture_state(Runtime& r) {
    // The published error remains available in the returned state on failure.
    (void)synchronize(r, CommandKind::kSnapshot);
    return get_state(r);
}
int notification_fd(const Runtime& r) {
    return r.notifications_fd;
}
void acknowledge_notifications(Runtime& r) {
    drain_fd(r.notifications_fd);
}
}  // namespace native_buttons
