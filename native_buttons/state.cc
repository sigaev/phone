#include "native_buttons/state.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>

#include "native_buttons/scene.h"

namespace native_buttons {
namespace {
constexpr std::uint32_t kMagic = 0x50454c49;
constexpr std::uint32_t kVersion = 3;
struct StateRecord {
    std::uint32_t magic, version;
    std::int32_t count;
    std::uint32_t flags;
    float yaw, zoom;
    double time;
};
static_assert(sizeof(StateRecord) == sizeof(SavedState));
static_assert(offsetof(StateRecord, time) == 24);
struct LegacyRecord {
    std::uint32_t magic, version;
    std::int32_t count;
    std::uint32_t flags;
    float yaw, time, zoom;
};
static_assert(sizeof(LegacyRecord) == 28);
}  // namespace

SavedState encode_state(const SessionState& state) {
    StateRecord record{
        kMagic,    kVersion,   state.count, (state.maximum ? 1u : 0u) | (state.paused ? 2u : 0u),
        state.yaw, state.zoom, state.time};
    SavedState bytes;
    std::memcpy(bytes.data(), &record, sizeof(record));
    return bytes;
}

common::Result<SessionState> decode_state(std::span<const std::byte> bytes) {
    // Accept the count-only format saved by previous versions of the app.
    if (bytes.size() == sizeof(std::int32_t)) {
        std::int32_t count;
        std::memcpy(&count, bytes.data(), sizeof(count));
        return SessionState{.count = std::clamp(int(count), 0, 999999)};
    }
    bool legacy = bytes.size() == 24 || bytes.size() == sizeof(LegacyRecord);
    if (!legacy && bytes.size() != sizeof(StateRecord))
        return std::unexpected(common::Error{"Invalid Activity state size"});
    StateRecord record{};
    if (legacy) {
        LegacyRecord previous{.zoom = 1};
        std::memcpy(&previous, bytes.data(), bytes.size());
        if (previous.version != (bytes.size() == 24 ? 1u : 2u))
            return std::unexpected(common::Error{"Invalid or unsupported Activity state"});
        record = {previous.magic, kVersion,      previous.count, previous.flags,
                  previous.yaw,   previous.zoom, previous.time};
    } else {
        std::memcpy(&record, bytes.data(), bytes.size());
    }
    if (record.magic != kMagic || record.version != kVersion || record.count < 0 ||
        record.count > 999999 || (record.flags & ~3u) || !std::isfinite(record.yaw) ||
        !std::isfinite(record.time) || record.time < 0 || !std::isfinite(record.zoom) ||
        record.zoom < kMinimumZoom || record.zoom > kMaximumZoom)
        return std::unexpected(common::Error{"Invalid or unsupported Activity state"});
    return SessionState{
        record.count, bool(record.flags & 1), bool(record.flags & 2), record.yaw, record.time,
        record.zoom};
}
}  // namespace native_buttons
