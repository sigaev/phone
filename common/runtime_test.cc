#include <sys/resource.h>
#include <sys/wait.h>
#include <unistd.h>

#include <cerrno>
#include <chrono>
#include <csignal>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <limits>
#include <new>
#include <string>
#include <vector>

#include "common/owner.h"
#include "common/result.h"

namespace {
// Allocator calls are interposed by the linker, outside the compiler's view.
volatile bool fail_allocations = false;
int handler_calls = 0;

struct Allocation {
  int value = 7;
};

struct alignas(64) AlignedAllocation {
  int value = 9;
};

void destroy(Allocation* allocation) noexcept { delete allocation; }

void destroy(AlignedAllocation* allocation) noexcept { delete allocation; }

[[gnu::noinline]] common::Owner<Allocation> make_allocation() {
  return common::Owner<Allocation>(new Allocation);
}

void recover_allocation() {
  ++handler_calls;
  fail_allocations = false;
}

enum class Failure {
  kAllocation,
  kStringLength,
};

common::Result<void> expect_abort(Failure failure) {
  auto child = fork();
  if (child < 0) return std::unexpected(common::Error{"Cannot fork the runtime failure check"});
  if (child == 0) {
    const rlimit limit{0, 0};
    setrlimit(RLIMIT_CORE, &limit);
    close(STDERR_FILENO);
    if (failure == Failure::kAllocation) {
      fail_allocations = true;
      auto allocation = make_allocation();
      _exit(allocation->value);
    }
    std::string value;
    value.reserve(std::numeric_limits<std::size_t>::max());
    _exit(0);
  }
  int status = 0;
  pid_t waited;
  do { waited = waitpid(child, &status, 0); } while (waited < 0 && errno == EINTR);
  if (waited != child || !WIFSIGNALED(status) || WTERMSIG(status) != SIGABRT)
    return std::unexpected(common::Error{"Runtime failure did not abort directly"});
  return {};
}

common::Result<void> check_runtime() {
  auto old_handler = std::set_new_handler(nullptr);
  fail_allocations = true;
  common::Owner<Allocation> failed(new (std::nothrow) Allocation);
  common::Owner<AlignedAllocation> failed_aligned(new (std::nothrow) AlignedAllocation);
  fail_allocations = false;
  if (failed || failed_aligned)
    return std::unexpected(common::Error{"nothrow allocation must return null on failure"});

  std::set_new_handler(recover_allocation);
  fail_allocations = true;
  common::Owner<Allocation> recovered(new (std::nothrow) Allocation);
  std::set_new_handler(nullptr);
  if (!recovered || recovered->value != 7 || handler_calls != 1)
    return std::unexpected(common::Error{"Allocation handler did not recover and retry"});

  common::Owner<AlignedAllocation> aligned(new (std::nothrow) AlignedAllocation);
  if (!aligned || aligned->value != 9 ||
      reinterpret_cast<std::uintptr_t>(aligned.get()) % alignof(AlignedAllocation) != 0)
    return std::unexpected(common::Error{"Aligned allocation is invalid"});

  std::string original(128, 'x');
  std::string copied = original;
  copied.append(256, 'y');
  std::vector<int> values;
  for (int i = 0; i < 4096; ++i) values.push_back(i);
  if (original.size() != 128 || copied.size() != 384 || copied.back() != 'y' ||
      values.back() != 4095 || std::chrono::steady_clock::now().time_since_epoch().count() <= 0)
    return std::unexpected(common::Error{"Standard library smoke check failed"});

  if (auto result = expect_abort(Failure::kAllocation); !result) return result;
  if (auto result = expect_abort(Failure::kStringLength); !result) return result;
  std::set_new_handler(old_handler);
  return {};
}
}

extern "C" void* __real_malloc(std::size_t size);
extern "C" int __real_posix_memalign(void** pointer, std::size_t alignment, std::size_t size);

extern "C" void* __wrap_malloc(std::size_t size) {
  return fail_allocations ? nullptr : __real_malloc(size);
}

extern "C" int __wrap_posix_memalign(void** pointer, std::size_t alignment, std::size_t size) {
  return fail_allocations ? ENOMEM : __real_posix_memalign(pointer, alignment, size);
}

int main() {
  if (auto result = check_runtime(); !result) {
    std::fprintf(stderr, "%s\n", result.error().message.c_str());
    return 1;
  }
  std::puts("Allocation failure, alignment, handlers, strings, vectors, clocks and aborts passed");
  return 0;
}
