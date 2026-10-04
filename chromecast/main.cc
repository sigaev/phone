#include <android/bitmap.h>
#include <android/choreographer.h>
#include <android/configuration.h>
#include <android/input.h>
#include <android/log.h>
#include <android/looper.h>
#include <android/native_activity.h>
#include <android/native_window.h>
#include <stdlib.h>
#include <sys/timerfd.h>
#include <time.h>
#include <unistd.h>

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <new>
#include <string>
#include <vector>

#include "chromecast/android_platform.h"
#include "chromecast/app.h"
#include "common/gpu/renderer.h"

namespace {
using common::Owner;
using namespace chromecast;

// Android can resize or rotate a window after its last callback, so visible
// windows are checked on this interval.
constexpr long kTickNanoseconds = 100000000;

// Frame pacing, in CLOCK_MONOTONIC nanoseconds. The display stays at its peak
// refresh rate. Android throttles an app's Choreographer to the rate the app
// presents at, so Choreographer only describes the upcoming vsyncs: a timer
// starts each frame just in time for its slot on an exact grid N refreshes
// apart, emulating the peak rate divided by N. Timeline estimates vary slightly,
// so slots and requested presentation times allow this slack.
constexpr int64_t kVsyncSlack = 2000000;
// Beyond the average CPU and GPU time, a frame starts at least this early. Each
// late frame adds kLeadStep, up to a refresh, since GPU time varies; the lead
// decays by kLeadDecay per frame on time.
constexpr int64_t kCostMargin = 2000000, kLeadStep = 2000000, kLeadDecay = 20000;
// Down to 15 fps at 120 Hz.
constexpr unsigned kMaximumInterval = 8;
// N is the smallest interval whose budget, allowing kOverlap for the GPU
// overlapping consecutive frames, fits the frame cost. A new surface, detail
// level, or peak rate starts at the peak rate, where GPU clocks run highest, and
// measures after kMeasureFrames, or up to twice as long while the cost is still
// falling from the renderer's startup; cheaper frames later lower the cost.
// kLateLimit missed deadlines within kPacingWindow frames, once the lead is a
// full refresh, set a floor one above N, so a few misses alone are tolerated.
// After a hold since N rose to it, the floor steps down each frame without
// misses. GPU clocks fall at slower rates, inflating the cost, so after a hold
// the next faster rate is measured again if the cost exceeds its budget by at
// most kRetryFit. Slowing down within kUnstable of speeding up doubles the hold.
constexpr float kOverlap = 1.2f, kRetryFit = 1.25f;
constexpr unsigned kPacingWindow = 120, kLateLimit = 4, kMeasureFrames = 30;
constexpr int64_t kFirstHold = 1000000000, kLastHold = 32000000000, kUnstable = 5000000000;
// Late reports lag the display, and display mode changes shift presentation
// times, so both ignore this many frames.
constexpr unsigned kSettleFrames = 8;

struct Activity {
  ANativeActivity* activity = nullptr;
  AInputQueue* input = nullptr;
  Owner<Platform> platform;
  Owner<App> app;
  AChoreographer* choreographer = nullptr;
  int timer = -1, frame_timer = -1;
  ANativeWindow* window = nullptr;
  int64_t peak_period = 0, fastest_vsync = 0, vsync_period = 0, vsync_origin = 0, vsync_latch = 0,
          next_slot = 0;
  int64_t floor_until = 0, retry_at = 0, faster_at = 0, hold = kFirstHold, lead = kCostMargin;
  float cpu_ms = 0, best_cost = 0, previous_cost = 0;
  unsigned interval = 1, floor = 1, window_frames = 0, late_frames = 0, settling = 0, measuring = 0,
           measured = 0;
  // The frame callback cannot be cancelled; it frees a destroyed Activity.
  bool frame_pending = false, destroyed = false, finishing = false, resumed = false;
  int32_t pointer = -1;
  gpu::Rect content;
  // Insets can settle after the window callbacks; recheck them until then.
  double insets_until = 0;
};

void destroy(Activity* activity) noexcept {
  if (activity->timer >= 0) close(activity->timer);
  if (activity->frame_timer >= 0) close(activity->frame_timer);
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
  __android_log_print(ANDROID_LOG_ERROR, "chromecast", "%s", message.c_str());
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

// Dark system-bar icons over the light translucent veil, drawn edge to edge.
void style_system_bars(ANativeActivity* activity) {
  JNIEnv* env = activity->env;
  if (env->PushLocalFrame(8) != 0) {
    env->ExceptionClear();
    return;
  }
  jobject window = call_object(env, activity->clazz, "getWindow", "()Landroid/view/Window;");
  jobject controller =
      call_object(env, window, "getInsetsController", "()Landroid/view/WindowInsetsController;");
  constexpr int kLightBars = 8 | 16;
  call_void(env, controller, "setSystemBarsAppearance", "(II)V", kLightBars, kLightBars);
  env->PopLocalFrame(nullptr);
}

bool thrown(JNIEnv* env) {
  if (!env->ExceptionCheck()) return false;
  env->ExceptionClear();
  return true;
}

// System bar and display cutout insets from the decor view's root WindowInsets.
bool read_insets(JNIEnv* env, jobject insets, int (&sides)[4]) {
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
  if (ok) ok = read_insets(env, insets, sides);
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

// The fastest refresh rate the display can switch to seamlessly at its current
// resolution, or zero if Android does not report one.
float peak_refresh_rate(ANativeActivity* activity) {
  JNIEnv* env = activity->env;
  if (env->PushLocalFrame(8) != 0) {
    env->ExceptionClear();
    return 0;
  }
  auto call = [&](jobject object, const char* name, const char* signature) -> jobject {
    if (!object || env->ExceptionCheck()) return nullptr;
    jmethodID method = env->GetMethodID(env->GetObjectClass(object), name, signature);
    return method && !env->ExceptionCheck() ? env->CallObjectMethod(object, method) : nullptr;
  };
  jobject display = call(activity->clazz, "getDisplay", "()Landroid/view/Display;");
  jobject mode = call(display, "getMode", "()Landroid/view/Display$Mode;");
  jmethodID current = mode && !env->ExceptionCheck()
                          ? env->GetMethodID(env->GetObjectClass(mode), "getRefreshRate", "()F")
                          : nullptr;
  float peak = current && !env->ExceptionCheck() ? env->CallFloatMethod(mode, current) : 0;
  auto alternatives = static_cast<jfloatArray>(call(mode, "getAlternativeRefreshRates", "()[F"));
  if (alternatives && !env->ExceptionCheck()) {
    jsize count = env->GetArrayLength(alternatives);
    std::vector<jfloat> rates(count);
    env->GetFloatArrayRegion(alternatives, 0, count, rates.data());
    for (float rate : rates) peak = std::max(peak, rate);
  }
  if (env->ExceptionCheck()) {
    env->ExceptionClear();
    peak = 0;
  }
  env->PopLocalFrame(nullptr);
  return peak;
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
  float peak = peak_refresh_rate(a.activity);
  a.peak_period = peak > 0 ? int64_t(1e9 / peak) : 0;
  if (a.window && peak > 0)
    ANativeWindow_setFrameRate(a.window, peak, ANATIVEWINDOW_FRAME_RATE_COMPATIBILITY_FIXED_SOURCE);
}

int64_t monotonic_nanoseconds() {
  timespec now;
  clock_gettime(CLOCK_MONOTONIC, &now);
  return int64_t(now.tv_sec) * 1000000000 + now.tv_nsec;
}

int64_t fastest_period(const Activity& r) {
  return r.peak_period ? r.peak_period : r.fastest_vsync;
}

int64_t period(const Activity& r, unsigned interval) {
  return std::max<int64_t>(interval * fastest_period(r), r.vsync_period);
}

// Ask Android for the peak rate as fixed-rate content, so it keeps the display there.
void request_frame_rate(Activity& r) {
  if (r.window && fastest_period(r))
    ANativeWindow_setFrameRate(r.window, 1e9f / fastest_period(r),
                               ANATIVEWINDOW_FRAME_RATE_COMPATIBILITY_FIXED_SOURCE);
}

// CPU and GPU work overlap across frames, so the slower one limits the rate.
float frame_cost(const Activity& r) {
  return std::max(r.cpu_ms, gpu::get_stats(*get_renderer(*r.app)).gpu_ms) * 1e6f;
}

// From a frame's start to its presentation: the CPU and GPU work, which can run
// back to back, and the display's own latency.
int64_t frame_latency(const Activity& r) {
  return r.vsync_latch + r.lead +
         int64_t((r.cpu_ms + gpu::get_stats(*get_renderer(*r.app)).gpu_ms) * 1e6f);
}

// The first predicted vsync at or after the given time.
int64_t vsync_after(const Activity& r, int64_t time) {
  int64_t refreshes = (time - r.vsync_origin + r.vsync_period - 1) / r.vsync_period;
  if (time < r.vsync_origin) refreshes = -((r.vsync_origin - time) / r.vsync_period);
  return r.vsync_origin + refreshes * r.vsync_period;
}

void change_interval(Activity& r, unsigned interval) {
  r.interval = interval;
  r.window_frames = r.late_frames = 0;
  r.settling = kSettleFrames;
}

// Each surface and detail level starts at the peak rate and measures its cost.
void reset_pacing(Activity& r) {
  change_interval(r, 1);
  r.floor = 1;
  r.best_cost = 0;
  r.measuring = kMeasureFrames;
  r.measured = 0;
  r.hold = kFirstHold;
  r.faster_at = r.floor_until = r.retry_at = 0;
  r.lead = kCostMargin;
  request_frame_rate(r);
}

void pace(Activity& r, int64_t slot, unsigned late) {
  // Late frames first start earlier; only lateness with a full refresh of lead
  // counts toward a slower rate.
  int64_t full_lead = std::max(r.vsync_period, kCostMargin);
  if (r.settling) {
    --r.settling;
  } else if (!late) {
    r.lead = std::max(kCostMargin, r.lead - kLeadDecay);
  } else if (r.lead < full_lead) {
    r.lead = std::min(full_lead, r.lead + kLeadStep * late);
  } else {
    r.late_frames += late;
  }
  float cost = frame_cost(r);
  if (r.measuring) {
    bool falling = cost < .99f * r.previous_cost && r.measured < 2 * kMeasureFrames;
    ++r.measured;
    if (r.measuring > 1 || !falling) --r.measuring;
    if (!r.measuring) r.best_cost = cost;
  } else {
    r.best_cost = std::min(r.best_cost, cost);
  }
  r.previous_cost = cost;
  if (r.late_frames >= kLateLimit) {
    r.floor = std::min(r.interval + 1, kMaximumInterval);
    r.window_frames = r.late_frames = 0;
  } else if (++r.window_frames == kPacingWindow) {
    r.window_frames = r.late_frames = 0;
  }
  if (r.floor > r.interval) r.floor_until = 0;
  else if (r.floor > 1 && !r.late_frames && slot >= r.floor_until) --r.floor;
  unsigned interval = r.interval;
  if (!r.measuring) {
    interval = 1;
    while (interval < kMaximumInterval && r.best_cost > kOverlap * period(r, interval)) ++interval;
  }
  interval = std::max(interval, r.floor);
  if (interval == r.interval && interval > 1 && r.floor < interval && !r.measuring &&
      !r.late_frames && slot >= r.retry_at &&
      r.best_cost <= kRetryFit * kOverlap * period(r, interval - 1)) {
    --interval;
    r.measuring = kMeasureFrames;
    r.measured = 0;
  }
  if (interval < r.interval) {
    r.faster_at = slot;
  } else if (interval > r.interval) {
    r.hold = r.faster_at && slot - r.faster_at < kUnstable ? std::min(r.hold * 2, kLastHold)
                                                           : kFirstHold;
    r.faster_at = 0;
    r.retry_at = slot + r.hold;
    if (!r.floor_until) r.floor_until = slot + r.hold;
    // An interrupted measurement restarts at the slower rate.
    if (r.measuring) {
      r.measuring = kMeasureFrames;
      r.measured = 0;
    }
  }
  if (interval != r.interval) change_interval(r, interval);
}

// The same presentation grid and measured-cost pacing as Native Buttons.
int64_t vsync_time(const AChoreographerFrameCallbackData* frame, size_t index) {
  return AChoreographerFrameCallbackData_getFrameTimelineExpectedPresentationTimeNanos(frame,
                                                                                       index);
}

void arm_frame(Activity& a) {
  itimerspec timer{};
  if (a.resumed && !a.finishing && a.vsync_period && update(*a.app, monotonic())) {
    int64_t now = monotonic_nanoseconds(), latency = frame_latency(a);
    if (!a.next_slot) a.next_slot = vsync_after(a, now + latency);
    int64_t start = std::max(a.next_slot - latency, now + 1);
    timer.it_value = {time_t(start / 1000000000), long(start % 1000000000)};
  } else a.next_slot = 0;
  timerfd_settime(a.frame_timer, TFD_TIMER_ABSTIME, &timer, nullptr);
}

void on_frame(const AChoreographerFrameCallbackData*, void* data);

void schedule(Activity& a) {
  if (a.frame_pending || a.destroyed || a.finishing || !a.resumed || !a.choreographer) return;
  a.frame_pending = true;
  AChoreographer_postVsyncCallback(a.choreographer, on_frame, &a);
}

// The clipboard's first item as text, or empty.
std::string read_clipboard(ANativeActivity* activity) {
  JNIEnv* env = activity->env;
  std::string result;
  if (env->PushLocalFrame(16) != 0) {
    env->ExceptionClear();
    return result;
  }
  jstring name = env->NewStringUTF("clipboard");
  jclass context = env->GetObjectClass(activity->clazz);
  jmethodID service =
      env->GetMethodID(context, "getSystemService", "(Ljava/lang/String;)Ljava/lang/Object;");
  jobject manager =
      service && name ? env->CallObjectMethod(activity->clazz, service, name) : nullptr;
  if (thrown(env)) manager = nullptr;
  jobject clip = call_object(env, manager, "getPrimaryClip", "()Landroid/content/ClipData;");
  bool ok = clip != nullptr;
  int count = call_int(env, clip, "getItemCount", ok);
  if (ok && count > 0) {
    jmethodID at = env->GetMethodID(env->GetObjectClass(clip), "getItemAt",
                                    "(I)Landroid/content/ClipData$Item;");
    jobject item = at ? env->CallObjectMethod(clip, at, 0) : nullptr;
    if (thrown(env)) item = nullptr;
    jmethodID coerce = item
                           ? env->GetMethodID(env->GetObjectClass(item), "coerceToText",
                                              "(Landroid/content/Context;)Ljava/lang/CharSequence;")
                           : nullptr;
    jobject text = coerce ? env->CallObjectMethod(item, coerce, activity->clazz) : nullptr;
    if (thrown(env)) text = nullptr;
    auto value = static_cast<jstring>(call_object(env, text, "toString", "()Ljava/lang/String;"));
    if (const char* chars = value ? env->GetStringUTFChars(value, nullptr) : nullptr) {
      result = chars;
      env->ReleaseStringUTFChars(value, chars);
    }
  }
  if (env->ExceptionCheck()) env->ExceptionClear();
  env->PopLocalFrame(nullptr);
  return result;
}

// Answer paste requests and draw any change on the next vsync.
void settle(Activity& a) {
  if (take_wallpaper_request(*a.app)) {
    JNIEnv* env = a.activity->env;
    if (env->PushLocalFrame(4) == 0) {
      jclass type = env->GetObjectClass(a.activity->clazz);
      jmethodID choose = env->GetMethodID(type, "chooseWallpaper", "()V");
      if (choose) env->CallVoidMethod(a.activity->clazz, choose);
      if (thrown(env)) report(a, "Cannot open the wallpaper picker", false);
      env->PopLocalFrame(nullptr);
    } else env->ExceptionClear();
  }
  if (take_paste_request(*a.app)) {
    std::string text = read_clipboard(a.activity);
    if (text.empty()) report(a, "The clipboard has no text", false);
    else paste(*a.app, text);
    std::fill(text.begin(), text.end(), '\0');
  }
  if (update(*a.app, monotonic())) schedule(a);
}

void on_frame(const AChoreographerFrameCallbackData* frame, void* data) {
  auto* a = static_cast<Activity*>(data);
  a->frame_pending = false;
  if (a->destroyed) {
    Owner<Activity> owner(a);
    return;
  }
  if (!a->resumed || a->finishing || !update(*a->app, monotonic())) return;
  size_t timelines = AChoreographerFrameCallbackData_getFrameTimelinesLength(frame);
  if (timelines == 0) {
    schedule(*a);
    return;
  }
  a->vsync_origin = vsync_time(frame, 0);
  a->vsync_latch =
      a->vsync_origin - AChoreographerFrameCallbackData_getFrameTimelineDeadlineNanos(frame, 0);
  if (timelines > 1) {
    int64_t refresh = vsync_time(frame, 1) - a->vsync_origin;
    if (std::llabs(refresh - a->vsync_period) > kVsyncSlack) {
      a->vsync_period = refresh;
      a->next_slot = 0;
      a->settling = std::max(a->settling, kSettleFrames);
    }
    if (!a->fastest_vsync || refresh < a->fastest_vsync - kVsyncSlack) {
      a->fastest_vsync = refresh;
      if (!a->peak_period) request_frame_rate(*a);
    }
  }
  if (!a->vsync_period && a->peak_period) a->vsync_period = a->peak_period;
  if (a->vsync_period) {
    if (!a->next_slot) arm_frame(*a);
  } else {
    gpu::set_present_time(*get_renderer(*a->app), a->vsync_origin - kVsyncSlack);
    if (auto result = draw(*a->app, a->vsync_origin * 1e-9); !result)
      report(*a, result.error().message, true);
  }
  if (update(*a->app, monotonic())) schedule(*a);
}

int on_frame_timer(int fd, int, void* data) {
  auto& a = *static_cast<Activity*>(data);
  std::uint64_t expirations;
  (void)!read(fd, &expirations, sizeof(expirations));
  if (!a.resumed || a.finishing || !a.next_slot || !update(*a.app, monotonic())) {
    a.next_slot = 0;
    return 1;
  }
  int64_t slot = a.next_slot, started = monotonic_nanoseconds(), step = period(a, a.interval);
  unsigned missed = 0;
  while (slot - a.vsync_latch - int64_t(a.cpu_ms * 1e6f) < started) {
    slot += step;
    missed = 1;
  }
  auto& renderer = *get_renderer(*a.app);
  gpu::set_present_time(renderer, slot - kVsyncSlack);
  // Animate at the intended display time, not the variable callback arrival time.
  auto result = draw(*a.app, slot * 1e-9);
  if (!result) {
    report(a, result.error().message, true);
    a.next_slot = 0;
    return 1;
  }
  float cpu_ms = (monotonic_nanoseconds() - started) / 1e6f;
  a.cpu_ms = a.cpu_ms ? a.cpu_ms * .85f + cpu_ms * .15f : cpu_ms;
  pace(a, slot, missed + gpu::take_late_frames(renderer));
  a.next_slot = vsync_after(a, slot + period(a, a.interval) - kVsyncSlack);
  arm_frame(a);
  if (update(*a.app, monotonic())) schedule(a);
  return 1;
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

int on_session_event(int, int, void* data) {
  auto& a = *static_cast<Activity*>(data);
  on_session(*a.app);
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

int handle_key(Activity& a, const AInputEvent* event) {
  int code = AKeyEvent_getKeyCode(event);
  bool down = AKeyEvent_getAction(event) == AKEY_EVENT_ACTION_DOWN;
  bool repeat = AKeyEvent_getRepeatCount(event) > 0;
  if (code == AKEYCODE_BACK) {
    // Back goes up a screen; on the first screen the Activity finishes as usual.
    if (get_screen(*a.app) == Screen::kDevices) return 0;
    if (down && !repeat) back(*a.app);
    return 1;
  }
  if (code == AKEYCODE_ENTER || code == AKEYCODE_NUMPAD_ENTER) {
    if (down && !repeat) enter(*a.app);
    return 1;
  }
  return 0;
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
      handled = handle_key(a, event);
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
  a.window = window;
  if (auto result = attach_window(*a.app, window); !result) {
    report(a, result.error().message, true);
    return;
  }
  reset_pacing(a);
  refresh_content(a);
  settle(a);
}

void on_window_destroyed(ANativeActivity* activity, ANativeWindow*) {
  auto& a = state(activity);
  cancel_touch(a);
  // NativeActivity requires drawing to stop before this callback returns.
  detach_window(*a.app);
  a.window = nullptr;
  a.next_slot = 0;
  arm_frame(a);
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
  a.resumed = true;
  a.next_slot = 0;
  reset_pacing(a);
  JNIEnv* env = activity->env;
  if (env->PushLocalFrame(4) == 0) {
    jclass animator = env->FindClass("android/animation/ValueAnimator");
    jmethodID enabled =
        animator ? env->GetStaticMethodID(animator, "areAnimatorsEnabled", "()Z") : nullptr;
    bool motion = enabled ? env->CallStaticBooleanMethod(animator, enabled) : true;
    if (thrown(env)) motion = false;
    set_wallpaper_motion(*a.app, motion);
    env->PopLocalFrame(nullptr);
  } else env->ExceptionClear();
  settle(a);
}

void on_pause(ANativeActivity* activity) {
  auto& a = state(activity);
  a.resumed = false;
  a.next_slot = 0;
  arm_frame(a);
  cancel_touch(a);
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
  ALooper_removeFd(looper, a->frame_timer);
  ALooper_removeFd(looper, app_fd(*a->app));
  // Joins the session's worker, which releases its Wi-Fi requests.
  a->app.reset();
  a->platform.reset();
  if (a->frame_pending) a->destroyed = true;
  else Owner<Activity> owner(a);
}

void wallpaper_ready(JNIEnv* env, jclass, jlong handle, jobject bitmap) {
  auto& a = *reinterpret_cast<Activity*>(handle);
  std::vector<unsigned char> pixels;
  AndroidBitmapInfo info{};
  if (bitmap) {
    void* source = nullptr;
    if (AndroidBitmap_getInfo(env, bitmap, &info) != ANDROID_BITMAP_RESULT_SUCCESS ||
        info.format != ANDROID_BITMAP_FORMAT_RGBA_8888 || info.width == 0 || info.height == 0 ||
        info.width > 2048 || info.height > 2048 ||
        AndroidBitmap_lockPixels(env, bitmap, &source) != ANDROID_BITMAP_RESULT_SUCCESS) {
      if (env->ExceptionCheck()) env->ExceptionClear();
      report(a, "Cannot read the wallpaper image", false);
      return;
    }
    std::size_t row = std::size_t(info.width) * 4;
    pixels.resize(row * info.height);
    for (unsigned y = 0; y < info.height; ++y)
      std::memcpy(pixels.data() + y * row,
                  static_cast<const unsigned char*>(source) + y * info.stride, row);
    AndroidBitmap_unlockPixels(env, bitmap);
  }
  if (auto result = set_wallpaper(*a.app, info.width, info.height, std::move(pixels)); !result)
    report(a, result.error().message, false);
  a.next_slot = 0;
  reset_pacing(a);
  arm_frame(a);
  settle(a);
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
  ANativeActivity_setWindowFormat(activity, WINDOW_FORMAT_RGBA_8888);
  auto platform = create_platform(activity->vm, activity->env, activity->clazz);
  if (!platform) {
    report(*a, platform.error().message, true);
    return;
  }
  a->platform = std::move(*platform);
  SessionConfig config;
  config.directory = activity->internalDataPath;
  auto app = create_app(*a->platform, config);
  if (!app) {
    report(*a, app.error().message, true);
    return;
  }
  a->app = std::move(*app);
  configure_input(*a);
  a->choreographer = AChoreographer_getInstance();
  a->timer = timerfd_create(CLOCK_MONOTONIC, TFD_NONBLOCK | TFD_CLOEXEC);
  a->frame_timer = timerfd_create(CLOCK_MONOTONIC, TFD_NONBLOCK | TFD_CLOEXEC);
  ALooper* looper = ALooper_forThread();
  bool registered = a->choreographer && a->timer >= 0 && a->frame_timer >= 0 && looper &&
                    ALooper_addFd(looper, a->frame_timer, ALOOPER_POLL_CALLBACK,
                                  ALOOPER_EVENT_INPUT, on_frame_timer, a.get()) == 1 &&
                    ALooper_addFd(looper, a->timer, ALOOPER_POLL_CALLBACK, ALOOPER_EVENT_INPUT,
                                  on_tick, a.get()) == 1 &&
                    ALooper_addFd(looper, app_fd(*a->app), ALOOPER_POLL_CALLBACK,
                                  ALOOPER_EVENT_INPUT, on_session_event, a.get()) == 1;
  if (!registered) {
    if (looper) {
      if (a->timer >= 0) ALooper_removeFd(looper, a->timer);
      if (a->frame_timer >= 0) ALooper_removeFd(looper, a->frame_timer);
      ALooper_removeFd(looper, app_fd(*a->app));
    }
    report(*a, "Cannot register the application's event sources", true);
    return;
  }
  style_system_bars(activity);
  JNIEnv* env = activity->env;
  if (env->PushLocalFrame(4) != 0) {
    env->ExceptionClear();
    ALooper_removeFd(looper, a->timer);
    ALooper_removeFd(looper, a->frame_timer);
    ALooper_removeFd(looper, app_fd(*a->app));
    report(*a, "Cannot initialize the wallpaper picker", true);
    return;
  }
  jclass type = env->GetObjectClass(activity->clazz);
  jfieldID native_state = env->GetFieldID(type, "nativeState", "J");
  JNINativeMethod methods[] = {{"wallpaperReady", "(JLandroid/graphics/Bitmap;)V",
                                reinterpret_cast<void*>(wallpaper_ready)}};
  bool ready = native_state && env->RegisterNatives(type, methods, 1) == JNI_OK;
  if (ready) env->SetLongField(activity->clazz, native_state, reinterpret_cast<jlong>(a.get()));
  if (thrown(env)) ready = false;
  env->PopLocalFrame(nullptr);
  if (!ready) {
    ALooper_removeFd(looper, a->timer);
    ALooper_removeFd(looper, a->frame_timer);
    ALooper_removeFd(looper, app_fd(*a->app));
    report(*a, "Cannot initialize the wallpaper picker", true);
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
  callbacks->onStart = on_start;
  callbacks->onStop = on_stop;
  callbacks->onPause = on_pause;
  callbacks->onResume = on_resume;
  callbacks->onConfigurationChanged = on_configuration_changed;
  callbacks->onDestroy = on_destroy;
  activity->instance = a.release();
}
