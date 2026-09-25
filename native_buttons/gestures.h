#pragma once

#include "common/owner.h"
#include "common/result.h"

struct AInputEvent;

namespace native_buttons {
struct Runtime;
struct Gestures;
common::Result<common::Owner<Gestures>> create_gestures(Runtime& runtime);
void destroy(Gestures* gestures) noexcept;
void handle_motion(Gestures& gestures, const AInputEvent& event);
void cancel_gestures(Gestures& gestures);
void set_gesture_slop(Gestures& gestures, float pixels);
}
