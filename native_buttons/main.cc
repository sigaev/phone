#include <android/choreographer.h>
#include <android/input.h>
#include <android/log.h>
#include <android/looper.h>
#include <android/native_activity.h>
#include <android/window.h>
#include <dlfcn.h>

#include <algorithm>
#include <cerrno>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <new>

#include "common/gpu/renderer.h"
#include "native_buttons/scene.h"

namespace {
using common::Error;
using common::Owner;
using common::Result;

struct App {
    ANativeActivity* activity = nullptr;
    AInputQueue* input = nullptr;
    AChoreographer* choreographer = nullptr;
    ARect content{};
    Owner<gpu::Renderer> renderer;
    Owner<native_buttons::Scene> scene;
    int count = 0;
    int pressed = 0;
    bool resumed = false;
    bool frame_pending = false;
    bool destroyed = false;
    bool maximum = false;
    bool paused = false;
    bool dragging = false;
    float yaw = .34f;
    float last_x = 0;
    float last_y = 0;
    float travel = 0;
    float time = 0;
    float fps = 0;
    long last_frame = 0;
    double fps_time = 0;
    unsigned fps_frames = 0;
};

void destroy(App* app) noexcept {
    delete app;
}
App* state(ANativeActivity* activity) {
    return static_cast<App*>(activity->instance);
}
void request_frame(App& app);

Result<int> load_count(const App& app) {
    char path[1024];
    std::snprintf(path, sizeof(path), "%s/count.txt", app.activity->internalDataPath);
    FILE* file = std::fopen(path, "r");
    if (!file) {
        if (errno == ENOENT)
            return 0;
        return std::unexpected(Error{"Cannot read the saved count"});
    }
    int count = 0;
    int fields = std::fscanf(file, "%d", &count);
    std::fclose(file);
    if (fields != 1)
        return std::unexpected(Error{"The saved count is invalid"});
    return std::clamp(count, 0, 999999);
}
Result<void> save_count(const App& app) {
    char path[1024];
    std::snprintf(path, sizeof(path), "%s/count.txt", app.activity->internalDataPath);
    FILE* file = std::fopen(path, "w");
    if (!file)
        return std::unexpected(Error{"Cannot open the saved count for writing"});
    int written = std::fprintf(file, "%d\n", app.count);
    int closed = std::fclose(file);
    if (written < 0 || closed != 0)
        return std::unexpected(Error{"Cannot save the count"});
    return {};
}
void save_at_callback_boundary(const App& app) {
    if (auto result = save_count(app); !result) {
        __android_log_print(ANDROID_LOG_ERROR, "native_buttons", "%s",
                            result.error().message.c_str());
    }
}
void report_failure(App& app, const Error& error) {
    __android_log_print(ANDROID_LOG_ERROR, "native_buttons", "%s", error.message.c_str());
    JNIEnv* env = app.activity->env;
    jclass toast = env->FindClass("android/widget/Toast");
    if (toast) {
        jmethodID make = env->GetStaticMethodID(
            toast, "makeText",
            "(Landroid/content/Context;Ljava/lang/CharSequence;I)Landroid/widget/Toast;");
        jstring message = env->NewStringUTF(error.message.c_str());
        if (make && message) {
            jobject object =
                env->CallStaticObjectMethod(toast, make, app.activity->clazz, message, 1);
            if (object) {
                jmethodID show = env->GetMethodID(toast, "show", "()V");
                if (show)
                    env->CallVoidMethod(object, show);
                env->DeleteLocalRef(object);
            }
        }
        if (message)
            env->DeleteLocalRef(message);
        env->DeleteLocalRef(toast);
    }
    if (env->ExceptionCheck())
        env->ExceptionClear();
    ANativeActivity_finish(app.activity);
}
void on_frame(long nanos, void* data) {
    App& app = *static_cast<App*>(data);
    app.frame_pending = false;
    // Native Choreographer callbacks cannot be cancelled. on_destroy transfers
    // the final owner to this already-scheduled callback, on the same UI thread.
    if (app.destroyed) {
        Owner<App> owner(&app);
        return;
    }
    if (!app.resumed || !app.scene)
        return;
    double delta = app.last_frame ? double(nanos - app.last_frame) / 1e9 : 1. / 60;
    app.last_frame = nanos;
    if (!app.paused)
        app.time += std::clamp(float(delta), 0.f, .1f);
    app.fps_time += delta;
    ++app.fps_frames;
    if (app.fps_time > .5) {
        app.fps = app.fps_frames / app.fps_time;
        app.fps_frames = 0;
        app.fps_time = 0;
    }
    auto stats = gpu::get_stats(*app.renderer);
    gpu::Rect safe{0, 0, float(stats.width), float(stats.height)};
    if (app.content.right > app.content.left && app.content.bottom > app.content.top) {
        float left = std::clamp(float(app.content.left), 0.f, safe.w);
        float top = std::clamp(float(app.content.top), 0.f, safe.h);
        safe = {left, top, std::clamp(float(app.content.right), left, safe.w) - left,
                std::clamp(float(app.content.bottom), top, safe.h) - top};
    }
    auto result = native_buttons::render_scene(*app.scene, app.time, app.yaw, app.maximum,
                                               app.count, app.pressed, app.fps, app.paused, safe);
    if (!result) {
        report_failure(app, result.error());
        return;
    }
    request_frame(app);
}
void request_frame(App& app) {
    if (app.resumed && app.scene && !app.destroyed && !app.frame_pending) {
        app.frame_pending = true;
        AChoreographer_postFrameCallback(app.choreographer, on_frame, &app);
    }
}
int on_input_ready(int, int, void* data) {
    App& app = *static_cast<App*>(data);
    AInputEvent* event = nullptr;
    while (app.input && AInputQueue_getEvent(app.input, &event) >= 0) {
        if (AInputQueue_preDispatchEvent(app.input, event))
            continue;
        int handled = 0;
        if (app.scene && AInputEvent_getType(event) == AINPUT_EVENT_TYPE_MOTION) {
            int action = AMotionEvent_getAction(event) & AMOTION_EVENT_ACTION_MASK;
            float x = AMotionEvent_getX(event, 0), y = AMotionEvent_getY(event, 0);
            int hit = native_buttons::hit_test(*app.scene, x, y);
            if (action == AMOTION_EVENT_ACTION_DOWN) {
                app.pressed = hit;
                app.dragging = hit == 0;
                app.last_x = x;
                app.last_y = y;
                app.travel = 0;
            } else if (action == AMOTION_EVENT_ACTION_MOVE) {
                float dx = x - app.last_x, dy = y - app.last_y;
                app.travel += std::fabs(dx) + std::fabs(dy);
                if (app.dragging) {
                    int width = gpu::get_stats(*app.renderer).width;
                    app.yaw =
                        std::remainder(app.yaw - dx / std::max(1, width) * gpu::kPi, 2 * gpu::kPi);
                }
                if (hit != app.pressed)
                    app.pressed = 0;
                app.last_x = x;
                app.last_y = y;
            } else if (action == AMOTION_EVENT_ACTION_UP) {
                if (app.pressed && hit == app.pressed && app.travel < 30) {
                    if (hit == 1 && app.count < 999999) {
                        ++app.count;
                        save_at_callback_boundary(app);
                    } else if (hit == 2) {
                        app.count = 0;
                        save_at_callback_boundary(app);
                    } else if (hit == 3) {
                        app.maximum = !app.maximum;
                    } else if (hit == 4) {
                        app.paused = !app.paused;
                    }
                }
                app.pressed = 0;
                app.dragging = false;
            } else if (action == AMOTION_EVENT_ACTION_CANCEL ||
                       action == AMOTION_EVENT_ACTION_POINTER_DOWN) {
                app.pressed = 0;
                app.dragging = false;
            }
            handled = 1;
        }
        AInputQueue_finishEvent(app.input, event, handled);
    }
    return 1;
}
void on_window_created(ANativeActivity* activity, ANativeWindow* window) {
    App& app = *state(activity);
    auto renderer = gpu::create_renderer(window, native_buttons::get_scene_shaders());
    if (!renderer) {
        report_failure(app, renderer.error());
        return;
    }
    app.renderer = std::move(*renderer);
    auto scene = native_buttons::create_scene(*app.renderer);
    if (!scene) {
        report_failure(app, scene.error());
        return;
    }
    app.scene = std::move(*scene);
    // This optional Android 11 API keeps the API-26 minimum valid.
    using SetRate = int (*)(ANativeWindow*, float, int8_t);
    auto set_rate = reinterpret_cast<SetRate>(dlsym(RTLD_DEFAULT, "ANativeWindow_setFrameRate"));
    if (set_rate)
        set_rate(window, 60.f, 0);
    request_frame(app);
}
void on_window_destroyed(ANativeActivity* activity, ANativeWindow*) {
    App& app = *state(activity);
    app.scene.reset();
    app.renderer.reset();
    app.last_frame = 0;
}
void on_window_redraw(ANativeActivity* activity, ANativeWindow*) {
    request_frame(*state(activity));
}
void on_input_created(ANativeActivity* activity, AInputQueue* input) {
    App& app = *state(activity);
    app.input = input;
    AInputQueue_attachLooper(input, ALooper_forThread(), ALOOPER_POLL_CALLBACK, on_input_ready,
                             &app);
}
void on_input_destroyed(ANativeActivity* activity, AInputQueue* input) {
    AInputQueue_detachLooper(input);
    App& app = *state(activity);
    app.input = nullptr;
    app.pressed = 0;
    app.dragging = false;
}
void on_content_changed(ANativeActivity* activity, const ARect* rect) {
    state(activity)->content = *rect;
}
void on_pause(ANativeActivity* activity) {
    App& app = *state(activity);
    app.resumed = false;
    app.last_frame = 0;
    app.pressed = 0;
    save_at_callback_boundary(app);
}
void on_resume(ANativeActivity* activity) {
    App& app = *state(activity);
    app.resumed = true;
    app.last_frame = 0;
    request_frame(app);
}
void on_destroy(ANativeActivity* activity) {
    Owner<App> app(state(activity));
    save_at_callback_boundary(*app);
    app->destroyed = true;
    app->scene.reset();
    app->renderer.reset();
    activity->instance = nullptr;
    if (app->frame_pending)
        app.release();
}
void* on_save_state(ANativeActivity* activity, size_t* size) {
    // NativeActivity owns this malloc allocation after the callback returns.
    int* count = static_cast<int*>(std::malloc(sizeof(int)));
    if (!count) {
        *size = 0;
        return nullptr;
    }
    *count = state(activity)->count;
    *size = sizeof(int);
    return count;
}
}  // namespace

extern "C" __attribute__((visibility("default"))) void ANativeActivity_onCreate(
    ANativeActivity* activity, void* saved, size_t size) {
    Owner<App> app(new (std::nothrow) App);
    if (!app) {
        ANativeActivity_finish(activity);
        return;
    }
    app->activity = activity;
    app->choreographer = AChoreographer_getInstance();
    if (auto count = load_count(*app); count) {
        app->count = *count;
    } else {
        __android_log_print(ANDROID_LOG_WARN, "native_buttons", "%s",
                            count.error().message.c_str());
    }
    if (saved && size == sizeof(int))
        std::memcpy(&app->count, saved, sizeof(int));
    app->count = std::clamp(app->count, 0, 999999);
    auto* callbacks = activity->callbacks;
    callbacks->onNativeWindowCreated = on_window_created;
    callbacks->onNativeWindowDestroyed = on_window_destroyed;
    callbacks->onNativeWindowResized = on_window_redraw;
    callbacks->onNativeWindowRedrawNeeded = on_window_redraw;
    callbacks->onInputQueueCreated = on_input_created;
    callbacks->onInputQueueDestroyed = on_input_destroyed;
    callbacks->onContentRectChanged = on_content_changed;
    callbacks->onPause = on_pause;
    callbacks->onResume = on_resume;
    callbacks->onDestroy = on_destroy;
    callbacks->onSaveInstanceState = on_save_state;
    activity->instance = app.release();  // Ownership returns through on_destroy.
    ANativeActivity_setWindowFlags(activity, AWINDOW_FLAG_KEEP_SCREEN_ON, 0);
    __android_log_print(ANDROID_LOG_INFO, "native_buttons", "3D pelican scene created");
}
