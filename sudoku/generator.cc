#include "sudoku/generator.h"

#include <pthread.h>
#include <sys/eventfd.h>
#include <unistd.h>

#include <atomic>
#include <cerrno>
#include <new>

namespace sudoku {
struct Generator {
  pthread_mutex_t mutex = PTHREAD_MUTEX_INITIALIZER;
  pthread_cond_t wanted = PTHREAD_COND_INITIALIZER;
  pthread_t thread{};
  bool started = false, stopping = false, ready = false;
  std::atomic<bool> cancelled{false};
  int fd = -1;
  std::uint64_t seed = 0;
  Puzzle puzzle;
};

namespace {
void* run(void* data) {
  auto& g = *static_cast<Generator*>(data);
  pthread_setname_np(pthread_self(), "sudoku-puzzles");
  pthread_mutex_lock(&g.mutex);
  while (!g.stopping) {
    if (g.ready) {
      pthread_cond_wait(&g.wanted, &g.mutex);
      continue;
    }
    std::uint64_t seed = g.seed;
    g.seed = seed * 6364136223846793005ull + 1442695040888963407ull;
    pthread_mutex_unlock(&g.mutex);
    auto puzzle = generate_puzzle(seed, g.cancelled);
    pthread_mutex_lock(&g.mutex);
    if (!puzzle) break;
    g.puzzle = *puzzle;
    g.ready = true;
    const std::uint64_t one = 1;
    while (write(g.fd, &one, sizeof(one)) < 0 && errno == EINTR) {}
  }
  pthread_mutex_unlock(&g.mutex);
  return nullptr;
}
}

common::Result<common::Owner<Generator>> create_generator(std::uint64_t seed) {
  common::Owner<Generator> generator(new (std::nothrow) Generator);
  if (!generator) return std::unexpected(common::Error{"Cannot allocate the puzzle generator"});
  generator->seed = seed;
  generator->fd = eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK);
  if (generator->fd < 0)
    return std::unexpected(common::Error{"Cannot create the puzzle notification channel"});
  if (pthread_create(&generator->thread, nullptr, run, generator.get()) != 0)
    return std::unexpected(common::Error{"Cannot start the puzzle generator"});
  generator->started = true;
  return generator;
}

void destroy(Generator* g) noexcept {
  if (g->started) {
    g->cancelled.store(true, std::memory_order_relaxed);
    pthread_mutex_lock(&g->mutex);
    g->stopping = true;
    pthread_cond_signal(&g->wanted);
    pthread_mutex_unlock(&g->mutex);
    pthread_join(g->thread, nullptr);
  }
  if (g->fd >= 0) close(g->fd);
  pthread_cond_destroy(&g->wanted);
  pthread_mutex_destroy(&g->mutex);
  delete g;
}

int generator_fd(const Generator& g) { return g.fd; }

bool puzzle_ready(Generator& g) {
  std::uint64_t value;
  while (read(g.fd, &value, sizeof(value)) < 0 && errno == EINTR) {}
  pthread_mutex_lock(&g.mutex);
  bool ready = g.ready;
  pthread_mutex_unlock(&g.mutex);
  return ready;
}

std::optional<Puzzle> take_puzzle(Generator& g) {
  std::uint64_t value;
  while (read(g.fd, &value, sizeof(value)) < 0 && errno == EINTR) {}
  pthread_mutex_lock(&g.mutex);
  std::optional<Puzzle> puzzle;
  if (g.ready) {
    puzzle = g.puzzle;
    g.ready = false;
    pthread_cond_signal(&g.wanted);
  }
  pthread_mutex_unlock(&g.mutex);
  return puzzle;
}
}
