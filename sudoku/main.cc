#include <android/api-level.h>
#include <android/choreographer.h>
#include <android/configuration.h>
#include <android/input.h>
#include <android/log.h>
#include <android/looper.h>
#include <android/native_activity.h>
#include <stdlib.h>
#include <sys/timerfd.h>
#include <time.h>
#include <unistd.h>

#include <algorithm>
#include <cstdint>
#include <new>
#include <string>

#include "sudoku/app.h"

namespace {
using common::Owner;
using namespace sudoku;

// Android can resize or rotate a window after its last callback, so visible
// windows are checked on this interval, which also advances the clock display.
constexpr long kTickNanoseconds = 100000000;

struct Activity {
  ANativeActivity* activity = nullptr;
  AInputQueue* input = nullptr;
  Owner<App> app;
  AChoreographer* choreographer = nullptr;
  int timer = -1;
  // The frame callback cannot be cancelled; it frees a destroyed Activity.
  bool frame_pending = false, destroyed = false, finishing = false;
  int32_t pointer = -1;
  gpu::Rect content;
  // Insets can settle after the window callbacks; recheck them until then.
  double insets_until = 0;
};

void destroy(Activity* activity) noexcept {
  if (activity->timer >= 0) close(activity->timer);
  delete activity;
}

struct Configuration {
  AConfiguration* handle;
};

void destroy(Configuration* configuration) noexcept {
  if (configuration->handle) AConfiguration_delete(configuration->handle);
  delete configuration;
}

Activity& state(ANativeActivity* activity) { return *static_cast<Activity*>(activity->instance); }

double monotonic() {
  timespec now;
  clock_gettime(CLOCK_MONOTONIC, &now);
  return double(now.tv_sec) + now.tv_nsec * 1e-9;
}

void report(Activity& a, const std::string& message, bool fatal) {
  __android_log_print(ANDROID_LOG_ERROR, "sudoku", "%s", message.c_str());
  JNIEnv* env = a.activity->env;
  if (env->PushLocalFrame(8) == 0) {
    jclass toast = env->FindClass("android/widget/Toast");
    jmethodID make =
        toast ? env->GetStaticMethodID(
                    toast, "makeText",
                    "(Landroid/content/Context;Ljava/lang/CharSequence;I)Landroid/widget/Toast;")
              : nullptr;
    jstring text = make ? env->NewStringUTF(message.c_str()) : nullptr;
    jobject object =
        text ? env->CallStaticObjectMethod(toast, make, a.activity->clazz, text, 1) : nullptr;
    jmethodID show = object ? env->GetMethodID(toast, "show", "()V") : nullptr;
    if (show) env->CallVoidMethod(object, show);
    if (env->ExceptionCheck()) env->ExceptionClear();
    env->PopLocalFrame(nullptr);
  }
  if (env->ExceptionCheck()) env->ExceptionClear();
  if (fatal && !a.finishing) {
    a.finishing = true;
    ANativeActivity_finish(a.activity);
  }
}

// Minimal JNI calls on objects that may be null. Callers own a local frame.
jobject call_object(JNIEnv* env, jobject object, const char* name, const char* signature) {
  if (!object) return nullptr;
  jclass type = env->GetObjectClass(object);
  jmethodID method = env->GetMethodID(type, name, signature);
  jobject result = method ? env->CallObjectMethod(object, method) : nullptr;
  if (env->ExceptionCheck()) {
    env->ExceptionClear();
    return nullptr;
  }
  return result;
}

int call_int(JNIEnv* env, jobject object, const char* name, bool& ok) {
  if (!object) {
    ok = false;
    return 0;
  }
  jclass type = env->GetObjectClass(object);
  jmethodID method = env->GetMethodID(type, name, "()I");
  int result = method ? env->CallIntMethod(object, method) : 0;
  if (!method || env->ExceptionCheck()) {
    env->ExceptionClear();
    ok = false;
  }
  return result;
}

void call_void(JNIEnv* env, jobject object, const char* name, const char* signature, int first,
               int second = 0) {
  if (!object) return;
  jclass type = env->GetObjectClass(object);
  jmethodID method = env->GetMethodID(type, name, signature);
  if (method) env->CallVoidMethod(object, method, first, second);
  if (env->ExceptionCheck()) env->ExceptionClear();
}

// Dark system-bar icons over the white game; Android 15 draws edge to edge.
void style_system_bars(ANativeActivity* activity) {
  JNIEnv* env = activity->env;
  if (env->PushLocalFrame(8) != 0) {
    env->ExceptionClear();
    return;
  }
  jobject window = call_object(env, activity->clazz, "getWindow", "()Landroid/view/Window;");
  call_void(env, window, "setStatusBarColor", "(I)V", -1);
  call_void(env, window, "setNavigationBarColor", "(I)V", -1);
  if (android_get_device_api_level() >= 30) {
    jobject controller =
        call_object(env, window, "getInsetsController", "()Landroid/view/WindowInsetsController;");
    constexpr int kLightBars = 8 | 16;
    call_void(env, controller, "setSystemBarsAppearance", "(II)V", kLightBars, kLightBars);
  } else {
    jobject decor = call_object(env, window, "getDecorView", "()Landroid/view/View;");
    bool ok = true;
    int flags = call_int(env, decor, "getSystemUiVisibility", ok);
    constexpr int kLightStatusBar = 0x2000, kLightNavigationBar = 0x10;
    if (ok)
      call_void(env, decor, "setSystemUiVisibility", "(I)V",
                flags | kLightStatusBar | kLightNavigationBar);
  }
  env->PopLocalFrame(nullptr);
}

bool thrown(JNIEnv* env) {
  if (!env->ExceptionCheck()) return false;
  env->ExceptionClear();
  return true;
}

// Insets from the decor view's root WindowInsets on Android 11 and later.
bool modern_insets(JNIEnv* env, jobject insets, int (&sides)[4]) {
  jclass types = env->FindClass("android/view/WindowInsets$Type");
  if (thrown(env) || !types) return false;
  jmethodID bars = env->GetStaticMethodID(types, "systemBars", "()I");
  if (thrown(env) || !bars) return false;
  jmethodID cutout = env->GetStaticMethodID(types, "displayCutout", "()I");
  if (thrown(env) || !cutout) return false;
  int mask = env->CallStaticIntMethod(types, bars);
  if (thrown(env)) return false;
  mask |= env->CallStaticIntMethod(types, cutout);
  if (thrown(env)) return false;
  jmethodID get =
      env->GetMethodID(env->GetObjectClass(insets), "getInsets", "(I)Landroid/graphics/Insets;");
  if (thrown(env) || !get) return false;
  jobject values = env->CallObjectMethod(insets, get, mask);
  if (thrown(env) || !values) return false;
  jclass type = env->GetObjectClass(values);
  const char* names[] = {"left", "top", "right", "bottom"};
  for (int i = 0; i < 4; ++i) {
    jfieldID field = env->GetFieldID(type, names[i], "I");
    if (thrown(env) || !field) return false;
    sides[i] = env->GetIntField(values, field);
  }
  return true;
}

// System window insets and, from Android 9, cutouts on older releases.
bool legacy_insets(JNIEnv* env, jobject insets, int (&sides)[4]) {
  bool ok = true;
  const char* names[] = {"getSystemWindowInsetLeft", "getSystemWindowInsetTop",
                         "getSystemWindowInsetRight", "getSystemWindowInsetBottom"};
  for (int i = 0; i < 4; ++i) sides[i] = call_int(env, insets, names[i], ok);
  if (!ok || android_get_device_api_level() < 28) return ok;
  jobject cutout = call_object(env, insets, "getDisplayCutout", "()Landroid/view/DisplayCutout;");
  const char* safe[] = {"getSafeInsetLeft", "getSafeInsetTop", "getSafeInsetRight",
                        "getSafeInsetBottom"};
  bool known = cutout != nullptr;
  int values[4];
  for (int i = 0; i < 4 && known; ++i) values[i] = call_int(env, cutout, safe[i], known);
  if (known)
    for (int i = 0; i < 4; ++i) sides[i] = std::max(sides[i], values[i]);
  return true;
}

// The window area outside system bars and cutouts, or an empty rectangle if unknown.
gpu::Rect query_insets(ANativeActivity* activity) {
  JNIEnv* env = activity->env;
  if (env->PushLocalFrame(16) != 0) {
    env->ExceptionClear();
    return {};
  }
  jobject window = call_object(env, activity->clazz, "getWindow", "()Landroid/view/Window;");
  jobject decor = call_object(env, window, "getDecorView", "()Landroid/view/View;");
  jobject insets = call_object(env, decor, "getRootWindowInsets", "()Landroid/view/WindowInsets;");
  bool ok = insets != nullptr;
  int width = call_int(env, decor, "getWidth", ok), height = call_int(env, decor, "getHeight", ok);
  int sides[4] = {};
  if (ok)
    ok = android_get_device_api_level() >= 30 ? modern_insets(env, insets, sides)
                                              : legacy_insets(env, insets, sides);
  env->PopLocalFrame(nullptr);
  int horizontal = sides[0] + sides[2], vertical = sides[1] + sides[3];
  if (!ok || width <= horizontal || height <= vertical) return {};
  return {float(sides[0]), float(sides[1]), float(width - horizontal), float(height - vertical)};
}

void refresh_content(Activity& a) {
  a.insets_until = monotonic() + 1;
  gpu::Rect content = a.content, insets = query_insets(a.activity);
  if (insets.w > 0 && insets.h > 0) {
    if (content.w > 0 && content.h > 0) {
      float left = std::max(content.x, insets.x), top = std::max(content.y, insets.y);
      float right = std::min(content.x + content.w, insets.x + insets.w);
      float bottom = std::min(content.y + content.h, insets.y + insets.h);
      content = {left, top, std::max(0.f, right - left), std::max(0.f, bottom - top)};
    } else {
      content = insets;
    }
  }
  set_content(*a.app, content);
}

void configure_input(Activity& a) {
  JNIEnv* env = a.activity->env;
  int slop = 0;
  if (env->PushLocalFrame(8) == 0) {
    jclass type = env->FindClass("android/view/ViewConfiguration");
    jmethodID get =
        type ? env->GetStaticMethodID(type, "get",
                                      "(Landroid/content/Context;)Landroid/view/ViewConfiguration;")
             : nullptr;
    jobject configuration =
        get ? env->CallStaticObjectMethod(type, get, a.activity->clazz) : nullptr;
    bool ok = !env->ExceptionCheck();
    if (!ok) env->ExceptionClear();
    if (ok && configuration) slop = call_int(env, configuration, "getScaledTouchSlop", ok);
    if (!ok) slop = 0;
    env->PopLocalFrame(nullptr);
  }
  if (env->ExceptionCheck()) env->ExceptionClear();
  Owner<Configuration> resources(new (std::nothrow) Configuration{AConfiguration_new()});
  int density = ACONFIGURATION_DENSITY_MEDIUM;
  if (resources && resources->handle) {
    AConfiguration_fromAssetManager(resources->handle, a.activity->assetManager);
    density = AConfiguration_getDensity(resources->handle);
    if (density <= 0 || density >= ACONFIGURATION_DENSITY_ANY)
      density = ACONFIGURATION_DENSITY_MEDIUM;
  }
  float scale = float(density) / ACONFIGURATION_DENSITY_MEDIUM;
  set_density(*a.app, scale);
  set_touch_slop(*a.app, slop > 0 ? float(slop) : 8 * scale);
}

void on_frame(long, void* data);

void schedule(Activity& a) {
  if (a.frame_pending || a.destroyed || !a.choreographer) return;
  a.frame_pending = true;
  AChoreographer_postFrameCallback(a.choreographer, on_frame, &a);
}

// Show persistence problems and draw any change on the next vsync.
void settle(Activity& a) {
  auto notice = take_notice(*a.app);
  if (!notice.empty()) report(a, notice, false);
  if (update(*a.app, monotonic())) schedule(a);
}

void on_frame(long, void* data) {
  auto* a = static_cast<Activity*>(data);
  a->frame_pending = false;
  if (a->destroyed) {
    Owner<Activity> owner(a);
    return;
  }
  double now = monotonic();
  if (!update(*a->app, now)) return;
  if (auto drawn = draw(*a->app, now); !drawn) {
    report(*a, drawn.error().message, true);
    return;
  }
  // Deferred frames and animations continue on the next vsync.
  if (update(*a->app, now)) schedule(*a);
}

void set_ticking(Activity& a, bool ticking) {
  itimerspec spec{};
  if (ticking) spec.it_interval.tv_nsec = spec.it_value.tv_nsec = kTickNanoseconds;
  timerfd_settime(a.timer, 0, &spec, nullptr);
}

int on_tick(int, int, void* data) {
  auto& a = *static_cast<Activity*>(data);
  std::uint64_t expirations;
  (void)!read(a.timer, &expirations, sizeof(expirations));
  auto changed = check_surface(*a.app);
  if (!changed) {
    report(a, changed.error().message, true);
    return 1;
  }
  // Rotating by 180 degrees changes the cutout side without resizing the window.
  if (*changed) refresh_content(a);
  else if (monotonic() < a.insets_until) {
    double until = a.insets_until;
    refresh_content(a);
    a.insets_until = until;
  }
  settle(a);
  return 1;
}

int on_puzzle(int, int, void* data) {
  auto& a = *static_cast<Activity*>(data);
  on_puzzle_ready(*a.app, monotonic());
  settle(a);
  return 1;
}

void handle_motion(Activity& a, const AInputEvent* event, double now) {
  int32_t action = AMotionEvent_getAction(event);
  std::size_t index = (action & AMOTION_EVENT_ACTION_POINTER_INDEX_MASK) >>
                      AMOTION_EVENT_ACTION_POINTER_INDEX_SHIFT;
  auto tracked = [&]() -> int {
    for (std::size_t i = 0; i < AMotionEvent_getPointerCount(event); ++i)
      if (AMotionEvent_getPointerId(event, i) == a.pointer) return int(i);
    return -1;
  };
  auto send = [&](Touch kind, std::size_t i) {
    touch(*a.app, kind, AMotionEvent_getX(event, i), AMotionEvent_getY(event, i), now);
  };
  // Only the first finger interacts; later fingers are ignored.
  switch (action & AMOTION_EVENT_ACTION_MASK) {
    case AMOTION_EVENT_ACTION_DOWN:
      a.pointer = AMotionEvent_getPointerId(event, 0);
      send(Touch::kDown, 0);
      break;
    case AMOTION_EVENT_ACTION_MOVE:
      if (int i = tracked(); i >= 0) send(Touch::kMove, i);
      break;
    case AMOTION_EVENT_ACTION_UP:
      if (int i = tracked(); i >= 0) send(Touch::kUp, i);
      a.pointer = -1;
      break;
    case AMOTION_EVENT_ACTION_POINTER_UP:
      if (AMotionEvent_getPointerId(event, index) == a.pointer) {
        send(Touch::kUp, index);
        a.pointer = -1;
      }
      break;
    case AMOTION_EVENT_ACTION_CANCEL:
      touch(*a.app, Touch::kCancel, 0, 0, now);
      a.pointer = -1;
      break;
    default:
      break;
  }
}

int handle_key(Activity& a, const AInputEvent* event, double now) {
  int code = AKeyEvent_getKeyCode(event);
  bool down = AKeyEvent_getAction(event) == AKEY_EVENT_ACTION_DOWN;
  bool repeat = AKeyEvent_getRepeatCount(event) > 0;
  if (code == AKEYCODE_BACK) {
    // Back closes a dialog; otherwise the Activity finishes as usual.
    if (get_state(*a.app).dialog == Dialog::kNone) return 0;
    if (down && !repeat) dismiss(*a.app, now);
    return 1;
  }
  Key key = Key::kDigit;
  int digit = 0;
  if (code >= AKEYCODE_1 && code <= AKEYCODE_9) digit = code - AKEYCODE_0;
  else if (code >= AKEYCODE_NUMPAD_1 && code <= AKEYCODE_NUMPAD_9) digit = code - AKEYCODE_NUMPAD_0;
  else {
    switch (code) {
      case AKEYCODE_0:
      case AKEYCODE_NUMPAD_0:
      case AKEYCODE_DEL:
      case AKEYCODE_FORWARD_DEL:
        key = Key::kErase;
        break;
      case AKEYCODE_DPAD_LEFT:
        key = Key::kLeft;
        break;
      case AKEYCODE_DPAD_RIGHT:
        key = Key::kRight;
        break;
      case AKEYCODE_DPAD_UP:
        key = Key::kUp;
        break;
      case AKEYCODE_DPAD_DOWN:
        key = Key::kDown;
        break;
      case AKEYCODE_N:
        key = Key::kNotes;
        break;
      case AKEYCODE_U:
        key = Key::kUndo;
        break;
      case AKEYCODE_Z:
        if (!(AKeyEvent_getMetaState(event) & AMETA_CTRL_ON)) return 0;
        key = Key::kUndo;
        break;
      case AKEYCODE_P:
      case AKEYCODE_SPACE:
        key = Key::kPause;
        break;
      case AKEYCODE_ENTER:
      case AKEYCODE_NUMPAD_ENTER:
      case AKEYCODE_DPAD_CENTER:
        key = Key::kEnter;
        break;
      default:
        return 0;
    }
  }
  bool movement = key == Key::kLeft || key == Key::kRight || key == Key::kUp || key == Key::kDown;
  if (down && (!repeat || movement)) press(*a.app, key, digit, now);
  return 1;
}

int on_input(int, int, void* data) {
  auto& a = *static_cast<Activity*>(data);
  AInputEvent* event = nullptr;
  while (a.input && AInputQueue_getEvent(a.input, &event) >= 0) {
    if (AInputQueue_preDispatchEvent(a.input, event)) continue;
    int handled = 0;
    double now = monotonic();
    if (AInputEvent_getType(event) == AINPUT_EVENT_TYPE_MOTION) {
      handle_motion(a, event, now);
      handled = 1;
    } else if (AInputEvent_getType(event) == AINPUT_EVENT_TYPE_KEY) {
      handled = handle_key(a, event, now);
    }
    AInputQueue_finishEvent(a.input, event, handled);
  }
  settle(a);
  return 1;
}

void cancel_touch(Activity& a) {
  if (a.pointer >= 0) touch(*a.app, Touch::kCancel, 0, 0, monotonic());
  a.pointer = -1;
}

void on_window_created(ANativeActivity* activity, ANativeWindow* window) {
  auto& a = state(activity);
  if (auto result = attach_window(*a.app, window); !result) {
    report(a, result.error().message, true);
    return;
  }
  refresh_content(a);
  settle(a);
}

void on_window_destroyed(ANativeActivity* activity, ANativeWindow*) {
  auto& a = state(activity);
  cancel_touch(a);
  // NativeActivity requires drawing to stop before this callback returns.
  detach_window(*a.app);
}

void on_window_redraw(ANativeActivity* activity, ANativeWindow*) {
  auto& a = state(activity);
  refresh_content(a);
  if (auto result = redraw(*a.app, monotonic()); !result) report(a, result.error().message, true);
}

void on_window_resized(ANativeActivity* activity, ANativeWindow*) {
  auto& a = state(activity);
  cancel_touch(a);
  refresh_content(a);
  if (auto checked = check_surface(*a.app); !checked) report(a, checked.error().message, true);
  settle(a);
}

void on_content_changed(ANativeActivity* activity, const ARect* rect) {
  auto& a = state(activity);
  cancel_touch(a);
  a.content = {float(rect->left), float(rect->top), float(rect->right - rect->left),
               float(rect->bottom - rect->top)};
  refresh_content(a);
  settle(a);
}

void on_input_created(ANativeActivity* activity, AInputQueue* input) {
  auto& a = state(activity);
  a.input = input;
  AInputQueue_attachLooper(input, ALooper_forThread(), ALOOPER_POLL_CALLBACK, on_input, &a);
}

void on_input_destroyed(ANativeActivity* activity, AInputQueue* input) {
  AInputQueue_detachLooper(input);
  auto& a = state(activity);
  a.input = nullptr;
  cancel_touch(a);
}

void on_start(ANativeActivity* activity) {
  auto& a = state(activity);
  set_ticking(a, true);
  settle(a);
}

void on_stop(ANativeActivity* activity) { set_ticking(state(activity), false); }

void on_resume(ANativeActivity* activity) {
  auto& a = state(activity);
  set_resumed(*a.app, true, monotonic());
  settle(a);
}

void on_pause(ANativeActivity* activity) {
  auto& a = state(activity);
  cancel_touch(a);
  set_resumed(*a.app, false, monotonic());
  settle(a);
}

void on_configuration_changed(ANativeActivity* activity) {
  auto& a = state(activity);
  cancel_touch(a);
  configure_input(a);
  refresh_content(a);
  settle(a);
}

void on_destroy(ANativeActivity* activity) {
  auto* a = static_cast<Activity*>(activity->instance);
  activity->instance = nullptr;
  if (a->input) AInputQueue_detachLooper(a->input);
  ALooper* looper = ALooper_forThread();
  ALooper_removeFd(looper, a->timer);
  ALooper_removeFd(looper, puzzle_fd(*a->app));
  // Joins the puzzle generator and releases the game.
  a->app.reset();
  if (a->frame_pending) a->destroyed = true;
  else Owner<Activity> owner(a);
}
}

extern "C" __attribute__((visibility("default"))) void ANativeActivity_onCreate(
    ANativeActivity* activity, void*, size_t) {
  Owner<Activity> a(new (std::nothrow) Activity);
  if (!a) {
    ANativeActivity_finish(activity);
    return;
  }
  a->activity = activity;
  std::uint64_t seed;
  arc4random_buf(&seed, sizeof(seed));
  auto app = create_app(activity->internalDataPath, seed);
  if (!app) {
    report(*a, app.error().message, true);
    return;
  }
  a->app = std::move(*app);
  configure_input(*a);
  a->choreographer = AChoreographer_getInstance();
  a->timer = timerfd_create(CLOCK_MONOTONIC, TFD_NONBLOCK | TFD_CLOEXEC);
  ALooper* looper = ALooper_forThread();
  bool registered = a->choreographer && a->timer >= 0 && looper &&
                    ALooper_addFd(looper, a->timer, ALOOPER_POLL_CALLBACK, ALOOPER_EVENT_INPUT,
                                  on_tick, a.get()) == 1 &&
                    ALooper_addFd(looper, puzzle_fd(*a->app), ALOOPER_POLL_CALLBACK,
                                  ALOOPER_EVENT_INPUT, on_puzzle, a.get()) == 1;
  if (!registered) {
    if (looper) {
      if (a->timer >= 0) ALooper_removeFd(looper, a->timer);
      ALooper_removeFd(looper, puzzle_fd(*a->app));
    }
    report(*a, "Cannot register the application's event sources", true);
    return;
  }
  style_system_bars(activity);
  auto* callbacks = activity->callbacks;
  callbacks->onNativeWindowCreated = on_window_created;
  callbacks->onNativeWindowDestroyed = on_window_destroyed;
  callbacks->onNativeWindowResized = on_window_resized;
  callbacks->onNativeWindowRedrawNeeded = on_window_redraw;
  callbacks->onInputQueueCreated = on_input_created;
  callbacks->onInputQueueDestroyed = on_input_destroyed;
  callbacks->onContentRectChanged = on_content_changed;
  callbacks->onStart = on_start;
  callbacks->onStop = on_stop;
  callbacks->onPause = on_pause;
  callbacks->onResume = on_resume;
  callbacks->onConfigurationChanged = on_configuration_changed;
  callbacks->onDestroy = on_destroy;
  activity->instance = a.release();
}
