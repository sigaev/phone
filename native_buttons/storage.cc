#include "native_buttons/storage.h"

#include <fcntl.h>
#include <unistd.h>

#include <cerrno>
#include <charconv>
#include <cstdio>
#include <cstdlib>
#include <new>
#include <string>

#include "common/owner.h"

namespace native_buttons {
namespace {
struct PendingFile {
  int fd = -1;
  std::string path;
};

void destroy(PendingFile* file) noexcept {
  if (file->fd >= 0) close(file->fd);
  if (!file->path.empty()) unlink(file->path.c_str());
  delete file;
}

auto failure(const char* message) { return std::unexpected(common::Error{message}); }
}

common::Result<int> load_count(const char* directory) {
  std::string path = std::string(directory) + "/count.txt";
  int fd = open(path.c_str(), O_RDONLY | O_CLOEXEC);
  if (fd < 0) {
    if (errno == ENOENT) return 0;
    return failure("Cannot read the saved count");
  }
  char text[32];
  size_t size = 0;
  bool ok = true;
  while (size < sizeof(text)) {
    ssize_t n = read(fd, text + size, sizeof(text) - size);
    if (n < 0 && errno == EINTR) continue;
    if (n < 0) ok = false;
    if (n <= 0) break;
    size += n;
  }
  close(fd);
  if (!ok) return failure("Cannot read the saved count");
  if (!size || size == sizeof(text)) return failure("The saved count is invalid");
  int count = 0;
  auto parsed = std::from_chars(text, text + size, count);
  const char* end = parsed.ptr;
  while (end != text + size && (*end == '\n' || *end == '\r' || *end == ' ' || *end == '\t')) ++end;
  if (parsed.ec != std::errc{} || end != text + size || count < 0 || count > 999999)
    return failure("The saved count is invalid");
  return count;
}

common::Result<void> save_count(const char* directory, int count) {
  if (count < 0 || count > 999999) return failure("The count is outside its supported range");
  common::Owner<PendingFile> temporary(new (std::nothrow) PendingFile);
  if (!temporary) return failure("Cannot allocate saved-count state");
  temporary->path = std::string(directory) + "/count.XXXXXX";
  temporary->fd = mkstemp(temporary->path.data());
  if (temporary->fd < 0) {
    temporary->path.clear();
    return failure("Cannot create a temporary saved count");
  }
  char text[16];
  int length = std::snprintf(text, sizeof(text), "%d\n", count);
  for (int offset = 0; offset < length;) {
    ssize_t written = write(temporary->fd, text + offset, length - offset);
    if (written < 0 && errno == EINTR) continue;
    if (written <= 0) return failure("Cannot write the saved count");
    offset += written;
  }
  if (fsync(temporary->fd) != 0) return failure("Cannot flush the saved count");
  int closed = close(temporary->fd);
  temporary->fd = -1;
  if (closed != 0) return failure("Cannot close the saved count");
  std::string destination = std::string(directory) + "/count.txt";
  if (rename(temporary->path.c_str(), destination.c_str()) != 0)
    return failure("Cannot replace the saved count");
  temporary->path.clear();
  int dir = open(directory, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
  if (dir < 0) return failure("Cannot flush the saved-count directory");
  int synced = fsync(dir);
  close(dir);
  if (synced != 0) return failure("Cannot flush the saved-count directory");
  return {};
}
}
