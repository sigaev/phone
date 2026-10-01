#include "chromecast/storage.h"

#include <fcntl.h>
#include <unistd.h>

#include <cerrno>
#include <new>
#include <string>

#include "common/owner.h"

namespace chromecast {
namespace {
constexpr std::size_t kMaximumSize = 1 << 20;

struct File {
  int fd = -1;
  // A temporary file to remove unless it replaced its destination.
  std::string pending;
};

void destroy(File* file) noexcept {
  if (file->fd >= 0) close(file->fd);
  if (!file->pending.empty()) unlink(file->pending.c_str());
  delete file;
}

auto failure(const char* message) { return std::unexpected(common::Error{message}); }
}

common::Result<std::vector<std::byte>> load_file(const char* directory, const char* name) {
  std::string path = std::string(directory) + "/" + name;
  common::Owner<File> file(new (std::nothrow) File);
  if (!file) return failure("Cannot allocate file state");
  file->fd = open(path.c_str(), O_RDONLY | O_CLOEXEC);
  if (file->fd < 0) {
    if (errno == ENOENT) return std::vector<std::byte>{};
    return failure("Cannot open the saved device list");
  }
  std::vector<std::byte> bytes;
  std::byte buffer[4096];
  while (true) {
    ssize_t n = read(file->fd, buffer, sizeof(buffer));
    if (n < 0 && errno == EINTR) continue;
    if (n < 0) return failure("Cannot read the saved device list");
    if (n == 0) break;
    bytes.insert(bytes.end(), buffer, buffer + n);
    if (bytes.size() > kMaximumSize) return failure("The saved device list is too large");
  }
  return bytes;
}

common::Result<void> save_file(const char* directory, const char* name,
                               std::span<const std::byte> bytes) {
  common::Owner<File> file(new (std::nothrow) File);
  if (!file) return failure("Cannot allocate file state");
  file->pending = std::string(directory) + "/" + name + ".XXXXXX";
  file->fd = mkostemp(file->pending.data(), O_CLOEXEC);
  if (file->fd < 0) {
    file->pending.clear();
    return failure("Cannot create a temporary file");
  }
  for (std::size_t offset = 0; offset < bytes.size();) {
    ssize_t written = write(file->fd, bytes.data() + offset, bytes.size() - offset);
    if (written < 0 && errno == EINTR) continue;
    if (written <= 0) return failure("Cannot write the saved device list");
    offset += written;
  }
  if (fsync(file->fd) != 0) return failure("Cannot flush the saved device list");
  int closed = close(file->fd);
  file->fd = -1;
  if (closed != 0) return failure("Cannot close the saved device list");
  std::string destination = std::string(directory) + "/" + name;
  if (rename(file->pending.c_str(), destination.c_str()) != 0)
    return failure("Cannot replace the saved device list");
  file->pending.clear();
  int folder = open(directory, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
  if (folder < 0) return failure("Cannot flush the data directory");
  int synced = fsync(folder);
  close(folder);
  if (synced != 0) return failure("Cannot flush the data directory");
  return {};
}
}
