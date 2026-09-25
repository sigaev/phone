#include "native_buttons/gestures.h"

#include <android/input.h>

#include <cmath>
#include <new>

#include "native_buttons/runtime.h"

namespace native_buttons {
struct Gestures {
  Runtime* runtime;
  bool active = false, multitouch = false;
  int first_id = -1, second_id = -1;
  float span = 0, minimum_span = 16;
};

namespace {
void update_pinch(Gestures& g, const AInputEvent& event, int excluded, bool scale, size_t sample) {
  int first = -1, second = -1;
  int count = AMotionEvent_getPointerCount(&event);
  for (int i = 0; i < count; ++i) {
    if (i == excluded) continue;
    int id = AMotionEvent_getPointerId(&event, i);
    if (id == g.first_id) first = i;
    if (id == g.second_id) second = i;
  }
  bool same_pair = first >= 0 && second >= 0;
  if (!same_pair) {
    first = second = -1;
    for (int i = 0; i < count; ++i) {
      if (i == excluded) continue;
      if (first < 0) first = i;
      else {
        second = i;
        break;
      }
    }
  }
  if (second < 0) {
    g.span = 0;
    g.first_id = g.second_id = -1;
    return;
  }
  g.first_id = AMotionEvent_getPointerId(&event, first);
  g.second_id = AMotionEvent_getPointerId(&event, second);
  bool historical = sample < AMotionEvent_getHistorySize(&event);
  auto x = [&](int index) {
    return historical ? AMotionEvent_getHistoricalX(&event, index, sample)
                      : AMotionEvent_getX(&event, index);
  };
  auto y = [&](int index) {
    return historical ? AMotionEvent_getHistoricalY(&event, index, sample)
                      : AMotionEvent_getY(&event, index);
  };
  float span = std::hypot(x(first) - x(second), y(first) - y(second));
  if (!std::isfinite(span) || span < g.minimum_span) {
    g.span = 0;
    return;
  }
  if (scale && same_pair && g.span >= g.minimum_span) pinch(*g.runtime, span / g.span);
  g.span = span;
}
}

common::Result<common::Owner<Gestures>> create_gestures(Runtime& runtime) {
  common::Owner<Gestures> gestures(new (std::nothrow) Gestures{&runtime});
  if (!gestures) return std::unexpected(common::Error{"Cannot allocate touch gestures"});
  return gestures;
}

void destroy(Gestures* gestures) noexcept { delete gestures; }

void cancel_gestures(Gestures& g) {
  g.active = g.multitouch = false;
  g.span = 0;
  g.first_id = g.second_id = -1;
  touch(*g.runtime, Touch::kCancel, 0, 0);
}

void set_gesture_slop(Gestures& g, float pixels) {
  if (std::isfinite(pixels) && pixels > 0) {
    cancel_gestures(g);
    g.minimum_span = 2 * pixels;
  }
}

void handle_motion(Gestures& g, const AInputEvent& event) {
  int raw_action = AMotionEvent_getAction(&event);
  int action = raw_action & AMOTION_EVENT_ACTION_MASK;
  auto count = AMotionEvent_getPointerCount(&event);
  if (action == AMOTION_EVENT_ACTION_CANCEL || !count) {
    cancel_gestures(g);
    return;
  }
  if (action == AMOTION_EVENT_ACTION_DOWN) {
    cancel_gestures(g);
    g.active = true;
    touch(*g.runtime, Touch::kDown, AMotionEvent_getX(&event, 0), AMotionEvent_getY(&event, 0));
  } else if (!g.active) {
    return;
  } else if (action == AMOTION_EVENT_ACTION_UP) {
    if (!g.multitouch)
      touch(*g.runtime, Touch::kUp, AMotionEvent_getX(&event, 0), AMotionEvent_getY(&event, 0));
    cancel_gestures(g);
  } else if (action == AMOTION_EVENT_ACTION_POINTER_DOWN ||
             action == AMOTION_EVENT_ACTION_POINTER_UP) {
    g.multitouch = true;
    touch(*g.runtime, Touch::kCancel, 0, 0);
    int excluded = action == AMOTION_EVENT_ACTION_POINTER_UP
                       ? (raw_action & AMOTION_EVENT_ACTION_POINTER_INDEX_MASK) >>
                             AMOTION_EVENT_ACTION_POINTER_INDEX_SHIFT
                       : -1;
    update_pinch(g, event, excluded, false, AMotionEvent_getHistorySize(&event));
  } else if (action == AMOTION_EVENT_ACTION_MOVE) {
    if (count >= 2) {
      if (!g.multitouch) {
        g.multitouch = true;
        touch(*g.runtime, Touch::kCancel, 0, 0);
      }
      for (size_t sample = 0; sample <= AMotionEvent_getHistorySize(&event); ++sample)
        update_pinch(g, event, -1, true, sample);
    } else if (!g.multitouch) {
      // Android batches intermediate positions into MOVE events. Observe
      // them in order so leaving and returning to a button still cancels it.
      for (size_t sample = 0; sample < AMotionEvent_getHistorySize(&event); ++sample)
        touch(*g.runtime, Touch::kMove, AMotionEvent_getHistoricalX(&event, 0, sample),
              AMotionEvent_getHistoricalY(&event, 0, sample));
      touch(*g.runtime, Touch::kMove, AMotionEvent_getX(&event, 0), AMotionEvent_getY(&event, 0));
    }
    // Once a pinch starts, the remaining finger cannot become a button tap
    // or jump the orbit camera. A fresh DOWN starts the next gesture.
  }
}
}
