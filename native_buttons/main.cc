#include <android/input.h>
#include <android/log.h>
#include <android/looper.h>
#include <android/native_activity.h>
#include <android/window.h>

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <new>
#include <string>

#include "native_buttons/runtime.h"

namespace {
using common::Owner;
using namespace native_buttons;
struct App {
    ANativeActivity* activity = nullptr;
    AInputQueue* input = nullptr;
    Owner<Runtime> runtime;
    gpu::Rect content;
    std::string last_save_error;
    bool finishing = false;
};
void destroy(App* app) noexcept {
    delete app;
}
App* state(ANativeActivity* activity) {
    return static_cast<App*>(activity->instance);
}
void report(App& app, const std::string& message, bool fatal) {
    __android_log_print(ANDROID_LOG_ERROR, "native_buttons", "%s", message.c_str());
    JNIEnv* env = app.activity->env;
    jclass toast = env->FindClass("android/widget/Toast");
    if (toast) {
        jmethodID make = env->GetStaticMethodID(
            toast, "makeText",
            "(Landroid/content/Context;Ljava/lang/CharSequence;I)Landroid/widget/Toast;");
        jstring text = env->NewStringUTF(message.c_str());
        if (make && text) {
            jobject object = env->CallStaticObjectMethod(toast, make, app.activity->clazz, text, 1);
            if (object) {
                jmethodID show = env->GetMethodID(toast, "show", "()V");
                if (show)
                    env->CallVoidMethod(object, show);
                env->DeleteLocalRef(object);
            }
        }
        if (text)
            env->DeleteLocalRef(text);
        env->DeleteLocalRef(toast);
    }
    if (env->ExceptionCheck())
        env->ExceptionClear();
    if (fatal && !app.finishing) {
        app.finishing = true;
        ANativeActivity_finish(app.activity);
    }
}
int on_runtime_ready(int, int, void* data) {
    auto& app = *static_cast<App*>(data);
    acknowledge_notifications(*app.runtime);
    auto current = get_state(*app.runtime);
    if (!current.error.empty() && !app.finishing)
        report(app, current.error, true);
    if (!current.save_error.empty() && current.save_error != app.last_save_error)
        report(app, current.save_error, false);
    app.last_save_error = current.save_error;
    return 1;
}
int on_input_ready(int, int, void* data) {
    auto& app = *static_cast<App*>(data);
    AInputEvent* event = nullptr;
    while (app.input && AInputQueue_getEvent(app.input, &event) >= 0) {
        if (AInputQueue_preDispatchEvent(app.input, event))
            continue;
        int handled = 0;
        if (AInputEvent_getType(event) == AINPUT_EVENT_TYPE_MOTION) {
            int action = AMotionEvent_getAction(event) & AMOTION_EVENT_ACTION_MASK;
            float x = AMotionEvent_getX(event, 0), y = AMotionEvent_getY(event, 0);
            if (action == AMOTION_EVENT_ACTION_DOWN)
                touch(*app.runtime, Touch::kDown, x, y);
            else if (action == AMOTION_EVENT_ACTION_MOVE)
                touch(*app.runtime, Touch::kMove, x, y);
            else if (action == AMOTION_EVENT_ACTION_UP)
                touch(*app.runtime, Touch::kUp, x, y);
            else if (action == AMOTION_EVENT_ACTION_CANCEL ||
                     action == AMOTION_EVENT_ACTION_POINTER_DOWN)
                touch(*app.runtime, Touch::kCancel, x, y);
            handled = 1;
        } else if (AInputEvent_getType(event) == AINPUT_EVENT_TYPE_KEY) {
            int code = AKeyEvent_getKeyCode(event);
            bool select =
                code == AKEYCODE_ENTER || code == AKEYCODE_SPACE || code == AKEYCODE_DPAD_CENTER;
            bool next =
                code == AKEYCODE_TAB || code == AKEYCODE_DPAD_RIGHT || code == AKEYCODE_DPAD_DOWN;
            bool previous = code == AKEYCODE_DPAD_LEFT || code == AKEYCODE_DPAD_UP;
            handled = select || next || previous;
            if (handled && AKeyEvent_getAction(event) == AKEY_EVENT_ACTION_DOWN) {
                bool reverse = previous || (code == AKEYCODE_TAB &&
                                            (AKeyEvent_getMetaState(event) & AMETA_SHIFT_ON));
                if (!select || AKeyEvent_getRepeatCount(event) == 0)
                    key(*app.runtime, select    ? Key::kActivate
                                      : reverse ? Key::kPrevious
                                                : Key::kNext);
            }
        }
        AInputQueue_finishEvent(app.input, event, handled);
    }
    return 1;
}
void on_window_created(ANativeActivity* activity, ANativeWindow* window) {
    auto& app = *state(activity);
    if (auto result = set_surface(*app.runtime, window); !result)
        report(app, result.error().message, true);
}
void on_window_destroyed(ANativeActivity* activity, ANativeWindow*) {
    auto& app = *state(activity);
    // NativeActivity requires all drawing to stop before this callback returns.
    if (auto result = detach_surface(*app.runtime); !result && !app.finishing)
        report(app, result.error().message, true);
}
void on_window_redraw(ANativeActivity* activity, ANativeWindow*) {
    auto& app = *state(activity);
    // A redraw is required even while visible but paused (for example split screen).
    if (auto result = redraw(*app.runtime); !result)
        report(app, result.error().message, true);
}
void on_window_resized(ANativeActivity* activity, ANativeWindow*) {
    auto& app = *state(activity);
    set_content(*app.runtime, app.content);
}
void on_input_created(ANativeActivity* activity, AInputQueue* input) {
    auto& app = *state(activity);
    app.input = input;
    AInputQueue_attachLooper(input, ALooper_forThread(), ALOOPER_POLL_CALLBACK, on_input_ready,
                             &app);
}
void on_input_destroyed(ANativeActivity* activity, AInputQueue* input) {
    AInputQueue_detachLooper(input);
    auto& app = *state(activity);
    app.input = nullptr;
    touch(*app.runtime, Touch::kCancel, 0, 0);
}
void on_content_changed(ANativeActivity* activity, const ARect* rect) {
    auto& app = *state(activity);
    app.content = {float(rect->left), float(rect->top), float(rect->right - rect->left),
                   float(rect->bottom - rect->top)};
    set_content(*app.runtime, app.content);
}
void on_pause(ANativeActivity* activity) {
    set_resumed(*state(activity)->runtime, false);
}
void on_resume(ANativeActivity* activity) {
    set_resumed(*state(activity)->runtime, true);
}
void on_destroy(ANativeActivity* activity) {
    Owner<App> app(state(activity));
    if (app->input)
        AInputQueue_detachLooper(app->input);
    ALooper_removeFd(ALooper_forThread(), notification_fd(*app->runtime));
    activity->instance = nullptr;
    // Runtime joins its worker before freeing callback data or window ownership.
}
void* on_save_state(ANativeActivity* activity, size_t* size) {
    int* count = static_cast<int*>(std::malloc(sizeof(int)));
    if (!count) {
        *size = 0;
        return nullptr;
    }
    *count = capture_state(*state(activity)->runtime).count;
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
    int restored = -1;
    if (saved && size == sizeof(int)) {
        std::memcpy(&restored, saved, sizeof(int));
        restored = std::clamp(restored, 0, 999999);
    }
    auto runtime = create_runtime(activity->internalDataPath, restored);
    if (!runtime) {
        report(*app, runtime.error().message, true);
        return;
    }
    app->runtime = std::move(*runtime);
    if (ALooper_addFd(ALooper_forThread(), notification_fd(*app->runtime), ALOOPER_POLL_CALLBACK,
                      ALOOPER_EVENT_INPUT, on_runtime_ready, app.get()) != 1) {
        report(*app, "Cannot register application notifications", true);
        return;
    }
    auto* callbacks = activity->callbacks;
    callbacks->onNativeWindowCreated = on_window_created;
    callbacks->onNativeWindowDestroyed = on_window_destroyed;
    callbacks->onNativeWindowResized = on_window_resized;
    callbacks->onNativeWindowRedrawNeeded = on_window_redraw;
    callbacks->onInputQueueCreated = on_input_created;
    callbacks->onInputQueueDestroyed = on_input_destroyed;
    callbacks->onContentRectChanged = on_content_changed;
    callbacks->onPause = on_pause;
    callbacks->onResume = on_resume;
    callbacks->onDestroy = on_destroy;
    callbacks->onSaveInstanceState = on_save_state;
    activity->instance = app.release();
    ANativeActivity_setWindowFlags(activity, AWINDOW_FLAG_KEEP_SCREEN_ON, 0);
}
