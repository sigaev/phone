#include <sys/resource.h>
#include <sys/stat.h>
#include <unistd.h>

#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <string>

#include "native_buttons/runtime.h"
#include "native_buttons/storage.h"

namespace {
using common::Error;
using common::Result;
using namespace native_buttons;
#define CHECK(condition, message)                   \
    do {                                            \
        if (!(condition))                           \
            return std::unexpected(Error{message}); \
    } while (false)

Result<void> check_storage(const char* path) {
    auto missing = load_count(path);
    CHECK(missing && *missing == 0, "Missing count must start at zero");
    CHECK(save_count(path, 42), "Initial save failed");
    auto loaded = load_count(path);
    CHECK(loaded && *loaded == 42, "Saved count did not round trip");

    // A real failed write after opening the replacement must preserve the old file.
    rlimit original;
    CHECK(getrlimit(RLIMIT_FSIZE, &original) == 0, "Cannot read file-size limit");
    rlimit limited = original;
    limited.rlim_cur = 0;
    auto old_handler = std::signal(SIGXFSZ, SIG_IGN);
    CHECK(setrlimit(RLIMIT_FSIZE, &limited) == 0, "Cannot inject write failure");
    auto failed = save_count(path, 43);
    int restored = setrlimit(RLIMIT_FSIZE, &original);
    std::signal(SIGXFSZ, old_handler);
    CHECK(restored == 0, "Cannot restore file-size limit");
    CHECK(!failed, "Injected write failure unexpectedly succeeded");
    loaded = load_count(path);
    CHECK(loaded && *loaded == 42, "Failed save destroyed the previous count");
    CHECK(!save_count(path, -1) && !save_count(path, 1000000), "Invalid count was accepted");
    CHECK(save_count(path, 999999), "Maximum count save failed");
    loaded = load_count(path);
    CHECK(loaded && *loaded == 999999, "Maximum count did not round trip");
    return {};
}
Result<void> check_runtime(const char* path) {
    CHECK(save_count(path, 7), "Cannot seed application count");
    {
        auto runtime = create_runtime(path);
        CHECK(runtime, "Cannot create worker runtime");
        auto& r = **runtime;
        CHECK(capture_state(r).count == 7, "Lifecycle snapshot raced startup loading");
        key(r, Key::kNext);
        key(r, Key::kNext);
        key(r, Key::kNext);
        key(r, Key::kActivate);
        auto keyboard = capture_state(r);
        CHECK(keyboard.count == 8 && keyboard.focused == Control::kAdd,
              "Keyboard activation or lifecycle snapshot omitted queued input");
        key(r, Key::kPrevious);
        CHECK(capture_state(r).focused == Control::kPause, "Reverse keyboard focus failed");
        activate(r, Control::kReset);
        for (int i = 0; i < 7; ++i)
            activate(r, Control::kAdd);
        CHECK(set_surface(r, nullptr, 400, 720), "Cannot attach portrait target");
        CHECK(redraw(r), "Paused initial redraw failed");
        auto initial = get_state(r);
        CHECK(initial.count == 7 && initial.frames >= 2, "Worker did not load or redraw");
        CHECK(initial.safe.w == 400 && initial.safe.h == 720, "Wrong initial dimensions");

        touch(r, Touch::kDown, 100, 650);
        touch(r, Touch::kUp, 100, 650);
        CHECK(redraw(r), "Tap redraw failed");
        auto tapped = get_state(r);
        CHECK(tapped.count == 8 && tapped.saved, "Counter tap was not saved");
        touch(r, Touch::kDown, 100, 650);
        touch(r, Touch::kCancel, 100, 650);
        touch(r, Touch::kUp, 100, 650);
        touch(r, Touch::kDown, 100, 650);
        touch(r, Touch::kUp, 200, 650);
        CHECK(redraw(r), "Cancelled gesture redraw failed");
        CHECK(get_state(r).count == 8, "Cancel/large release displacement activated a button");

        touch(r, Touch::kDown, 10, 300);
        touch(r, Touch::kMove, 100, 300);
        touch(r, Touch::kCancel, 100, 300);
        activate(r, Control::kPause);
        focus_control(r, Control::kPause);
        CHECK(redraw(r), "Paused interaction redraw failed");
        auto paused = get_state(r);
        CHECK(paused.paused && paused.yaw != initial.yaw && paused.focused == Control::kPause,
              "Drag, pause or keyboard focus was not applied");
        CHECK(redraw(r), "Repeated paused redraw failed");
        auto next = get_state(r);
        CHECK(next.frames > paused.frames && next.time == paused.time,
              "Paused redraw must draw without advancing animation");

        activate(r, Control::kQuality);
        set_content(r, {10, 20, 380, 680});
        CHECK(redraw(r), "Quality change redraw failed");
        CHECK(get_state(r).maximum && get_state(r).safe.x == 10, "Quality/insets were not applied");
        CHECK(detach_surface(r), "Surface detach failed");
        unsigned detached_frames = get_state(r).frames;
        CHECK(redraw(r) && get_state(r).frames == detached_frames, "Detached runtime drew a frame");
        set_content(r, {});
        CHECK(set_surface(r, nullptr, 720, 320), "Cannot attach landscape target");
        CHECK(redraw(r), "Recreated landscape redraw failed");
        CHECK(get_state(r).safe.w == 720 && get_state(r).safe.h == 320,
              "Recreated target kept stale dimensions");
        activate(r, Control::kReset);
        set_resumed(r, false);
        CHECK(redraw(r), "Pause/save handshake failed");
        CHECK(get_state(r).count == 0 && get_state(r).saved, "Reset was not persisted");
    }
    auto loaded = load_count(path);
    CHECK(loaded && *loaded == 0, "Worker shutdown lost the saved state");
    // Destroy with a queued Choreographer callback to exercise callback lifetime.
    {
        auto runtime = create_runtime(path, 12);
        CHECK(runtime, "Cannot restore runtime");
        CHECK(set_surface(**runtime, nullptr, 160, 320), "Cannot attach restored target");
        set_resumed(**runtime, true);
        CHECK(redraw(**runtime), "Restored runtime failed");
        CHECK(get_state(**runtime).count == 12, "Saved instance state was not restored");
    }
    return {};
}
}  // namespace
int main() {
    std::string directory =
        std::string(std::getenv("TEST_TMPDIR") ? std::getenv("TEST_TMPDIR") : "/tmp") +
        "/native-buttons-test.XXXXXX";
    if (!mkdtemp(directory.data()))
        return 1;
    auto result = check_storage(directory.c_str());
    if (result)
        result = check_runtime(directory.c_str());
    unlink((directory + "/count.txt").c_str());
    rmdir(directory.c_str());
    if (!result) {
        std::fprintf(stderr, "%s\n", result.error().message.c_str());
        return 1;
    }
    std::puts(
        "Atomic persistence, failed writes, worker lifecycle, paused redraw, touch and recreation "
        "passed");
    return 0;
}
