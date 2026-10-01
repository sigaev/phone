#pragma once

#include <jni.h>

#include "chromecast/platform.h"
#include "common/owner.h"

namespace chromecast {
// Uses the application context of an Android Context from any thread.
common::Result<common::Owner<Platform>> create_platform(JavaVM* vm, JNIEnv* env, jobject context);
}
