#include "native_buttons/state.h"

#include <cmath>
#include <cstdint>
#include <cstring>

#include "native_buttons/scene.h"

namespace native_buttons {
namespace {
constexpr std::uint32_t kMagic = 0x50454c49;
constexpr std::uint32_t kVersion = 4;

struct StateRecord {
  std::uint32_t magic, version;
  std::int32_t count;
  std::uint32_t flags;
  float yaw, zoom;
  double time;
};

static_assert(sizeof(StateRecord) == sizeof(SavedState));
static_assert(offsetof(StateRecord, time) == 24);
}

SavedState encode_state(const SessionState& state) {
  unsigned bird_flag = state.bird == Bird::kFlamingo ? 4u : state.bird == Bird::kPelican ? 0u : 8u;
  StateRecord record{kMagic,      kVersion,
                     state.count, (state.maximum ? 1u : 0u) | (state.paused ? 2u : 0u) | bird_flag,
                     state.yaw,   state.zoom,
                     state.time};
  SavedState bytes;
  std::memcpy(bytes.data(), &record, sizeof(record));
  return bytes;
}

common::Result<SessionState> decode_state(std::span<const std::byte> bytes) {
  if (bytes.size() != sizeof(StateRecord))
    return std::unexpected(common::Error{"Invalid Activity state size"});
  StateRecord record;
  std::memcpy(&record, bytes.data(), bytes.size());
  if (record.magic != kMagic || record.version != kVersion || record.count < 0 ||
      record.count > 999999 || (record.flags & ~7u) || !std::isfinite(record.yaw) ||
      !std::isfinite(record.time) || record.time < 0 || !std::isfinite(record.zoom) ||
      record.zoom < kMinimumZoom || record.zoom > kMaximumZoom)
    return std::unexpected(common::Error{"Invalid or unsupported Activity state"});
  return SessionState{record.count,
                      bool(record.flags & 1),
                      bool(record.flags & 2),
                      record.yaw,
                      record.time,
                      record.zoom,
                      record.flags & 4 ? Bird::kFlamingo : Bird::kPelican};
}
}
