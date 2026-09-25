#include <android/input.h>

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <limits>
#include <vector>

#include "native_buttons/gestures.h"
#include "native_buttons/runtime.h"

struct Pointer {
  int id;
  float x, y;
};

struct AInputEvent {
  int action;
  std::vector<Pointer> pointers;
  std::vector<std::vector<Pointer>> history;
};

extern "C" {
int32_t AMotionEvent_getAction(const AInputEvent* event) { return event->action; }

size_t AMotionEvent_getPointerCount(const AInputEvent* event) { return event->pointers.size(); }

int32_t AMotionEvent_getPointerId(const AInputEvent* event, size_t index) {
  return event->pointers.at(index).id;
}

float AMotionEvent_getX(const AInputEvent* event, size_t index) {
  return event->pointers.at(index).x;
}

float AMotionEvent_getY(const AInputEvent* event, size_t index) {
  return event->pointers.at(index).y;
}

size_t AMotionEvent_getHistorySize(const AInputEvent* event) { return event->history.size(); }

float AMotionEvent_getHistoricalX(const AInputEvent* event, size_t index, size_t sample) {
  return event->history.at(sample).at(index).x;
}

float AMotionEvent_getHistoricalY(const AInputEvent* event, size_t index, size_t sample) {
  return event->history.at(sample).at(index).y;
}
}

namespace native_buttons {
struct Runtime {
  int downs = 0, moves = 0, ups = 0;
  bool pressed = false;
  std::vector<float> scales;
  std::vector<gpu::Vec3> positions;
};

void touch(Runtime& r, Touch action, float x, float y) {
  if (action == Touch::kDown) {
    ++r.downs;
    r.pressed = true;
  } else if (action == Touch::kMove) {
    ++r.moves;
    r.positions.push_back({x, y, 0});
  } else if (action == Touch::kUp) {
    ++r.ups;
  } else {
    r.pressed = false;
  }
}

void pinch(Runtime& r, float scale) { r.scales.push_back(scale); }
}

namespace {
void require(bool condition, const char* message) {
  if (!condition) {
    std::fprintf(stderr, "%s\n", message);
    std::abort();
  }
}
}

int main() {
  using namespace native_buttons;
  Runtime runtime;
  auto gestures = create_gestures(runtime);
  require(bool(gestures), "Cannot create gestures");
  auto send = [&](int action, std::initializer_list<Pointer> pointers) {
    AInputEvent event{action, pointers};
    handle_motion(**gestures, event);
  };
  auto lift = [&](int index, std::initializer_list<Pointer> pointers) {
    send(AMOTION_EVENT_ACTION_POINTER_UP | (index << AMOTION_EVENT_ACTION_POINTER_INDEX_SHIFT),
         pointers);
  };
  send(AMOTION_EVENT_ACTION_DOWN, {{7, 0, 0}});
  send(AMOTION_EVENT_ACTION_MOVE, {{7, 1, 1}});
  send(AMOTION_EVENT_ACTION_UP, {{7, 1, 1}});
  require(runtime.downs == 1 && runtime.moves == 1 && runtime.ups == 1,
          "Single-finger input changed");

  send(AMOTION_EVENT_ACTION_DOWN, {{7, 0, 0}});
  send(AMOTION_EVENT_ACTION_POINTER_DOWN, {{7, 0, 0}, {42, 100, 0}});
  require(!runtime.pressed, "Pinch retained a pending button tap");
  send(AMOTION_EVENT_ACTION_MOVE, {{42, 200, 0}, {7, 0, 0}});
  require(runtime.scales.size() == 1 && runtime.scales.back() == 2,
          "Spreading/reordered fingers did not zoom in");
  send(AMOTION_EVENT_ACTION_MOVE, {{7, 0, 0}, {42, 100, 0}});
  require(runtime.scales.back() == .5f, "Closing fingers did not zoom out");

  // Keep the original pair as a third finger arrives and array indices change.
  send(AMOTION_EVENT_ACTION_POINTER_DOWN, {{99, 400, 0}, {42, 100, 0}, {7, 0, 0}});
  send(AMOTION_EVENT_ACTION_MOVE, {{99, 600, 0}, {7, 0, 0}, {42, 200, 0}});
  require(runtime.scales.back() == 2 && runtime.scales.size() == 3,
          "Third finger changed the active pinch pair");
  lift(1, {{99, 600, 0}, {7, 0, 0}, {42, 200, 0}});
  require(runtime.scales.size() == 3, "Lifting a finger jumped the zoom");
  send(AMOTION_EVENT_ACTION_MOVE, {{42, 200, 0}, {99, 400, 0}});
  require(runtime.scales.back() == .5f, "Replacement pinch pair did not rebase");
  lift(0, {{42, 200, 0}, {99, 400, 0}});
  send(AMOTION_EVENT_ACTION_MOVE, {{99, 600, 0}});
  send(AMOTION_EVENT_ACTION_UP, {{99, 600, 0}});
  require(runtime.moves == 1 && runtime.ups == 1,
          "Remaining pinch finger became a drag or button tap");

  // Coincident fingers and invalid coordinates must not produce scale spikes.
  send(AMOTION_EVENT_ACTION_DOWN, {{7, 0, 0}});
  send(AMOTION_EVENT_ACTION_POINTER_DOWN, {{7, 0, 0}, {42, 0, 0}});
  send(AMOTION_EVENT_ACTION_MOVE, {{7, 0, 0}, {42, 4, 0}});
  send(AMOTION_EVENT_ACTION_MOVE, {{7, 0, 0}, {42, 100, 0}});
  require(runtime.scales.size() == 4, "Near-zero spans caused a zoom spike");
  send(AMOTION_EVENT_ACTION_MOVE, {{7, 0, 0}, {42, std::numeric_limits<float>::quiet_NaN(), 0}});
  send(AMOTION_EVENT_ACTION_MOVE, {{7, 0, 0}, {42, 200, 0}});
  require(runtime.scales.size() == 4, "Invalid span was retained as a pinch baseline");
  send(AMOTION_EVENT_ACTION_MOVE, {{7, 0, 0}, {42, 400, 0}});
  require(runtime.scales.back() == 2, "Pinch did not recover from an invalid span");

  cancel_gestures(**gestures);  // Rotation, input-queue loss or Activity pause.
  send(AMOTION_EVENT_ACTION_MOVE, {{7, 0, 0}, {42, 800, 0}});
  send(AMOTION_EVENT_ACTION_POINTER_DOWN, {{7, 0, 0}, {42, 800, 0}});
  require(runtime.scales.size() == 5, "Cancelled gesture continued after a lifecycle change");
  send(AMOTION_EVENT_ACTION_DOWN, {{7, 0, 0}});
  send(AMOTION_EVENT_ACTION_UP, {{7, 0, 0}});
  require(runtime.ups == 2, "A fresh tap did not recover after pinching");

  send(AMOTION_EVENT_ACTION_DOWN, {{7, 100, 100}});
  AInputEvent batched{
      AMOTION_EVENT_ACTION_MOVE, {{7, 100, 100}}, {{{7, 130, 100}}, {{7, 100, 140}}}};
  auto begin = runtime.positions.size();
  handle_motion(**gestures, batched);
  require(runtime.positions.size() == begin + 3 && runtime.positions[begin].x == 130 &&
              runtime.positions[begin + 1].y == 140 && runtime.positions[begin + 2].x == 100 &&
              runtime.positions[begin + 2].y == 100,
          "Batched movement was dropped or delivered out of order");
  send(AMOTION_EVENT_ACTION_POINTER_DOWN, {{7, 100, 100}, {42, 200, 100}});
  auto scales = runtime.scales.size();
  batched = {AMOTION_EVENT_ACTION_MOVE,
             {{42, 200, 100}, {7, 100, 100}},
             {{{42, 300, 100}, {7, 100, 100}}, {{42, 400, 100}, {7, 100, 100}}}};
  handle_motion(**gestures, batched);
  require(runtime.scales.size() == scales + 3 && runtime.scales[scales] == 2 &&
              runtime.scales[scales + 1] == 1.5f && runtime.scales[scales + 2] == 1.f / 3,
          "Batched pinch movement lost pointer identity or sample ordering");
  std::puts("Pinch direction, pointer identity, extra fingers, cancellation and taps passed");
}
