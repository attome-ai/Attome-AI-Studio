#pragma once
// Hierarchical zone profiler, built into the engine from the first module.
//
//   ATM_PROFILE_SCOPE("patch.apply");   // times the enclosing block
//   ATM_PROFILE_FRAME();                // one unit of work on this thread (one Tool call, later one video frame)
//
// Compile-time switch: ATM_PROFILING (CMake option ATTOME_PROFILING, default ON). With ATM_PROFILING=0 every
// macro compiles to nothing.
// Runtime switch: atm::prof::set_enabled(bool); when disabled a scope costs one relaxed load and a branch.
//
// Same model as the game engine's ATMFrameProfiler (zones form a tree by nesting; each zone keeps a smoothed
// per-frame average, a peak over the last second and calls per frame), changed for a daemon:
//   - every thread has its own zone tree, so the writer, I/O and (later) render threads are all measured;
//   - zones also keep lifetime totals (calls, total, min, max), because requests differ from each other;
//   - the hot path takes no lock and allocates nothing: zones live in a fixed per-thread array, counters are
//     relaxed atomics written only by the owning thread, time is the CPU timestamp counter where available;
//   - the data is read from any thread as JSON (Tool `profile.get`, `attome profile`, `attomed --profile-interval`).
//
// Zone names must be string literals (or other strings that live for the whole process).

#ifndef ATM_PROFILING
#define ATM_PROFILING 1
#endif

#include <atomic>
#include <cstdint>
#include <string>

#include <nlohmann/json_fwd.hpp>

#if ATM_PROFILING && (defined(_M_X64) || defined(__x86_64__))
#if defined(_MSC_VER)
#include <intrin.h>
#else
#include <x86intrin.h>
#endif
#define ATM_PROF_TSC 1
#else
#include <chrono>
#define ATM_PROF_TSC 0
#endif

namespace atm::prof {

inline constexpr int kMaxZones = 256; // per thread; further zones are not recorded
inline constexpr int kMaxDepth = 64;

// Names the OS thread (MODULES §A.2.6: every thread is named) and its profiler tree. A new thread that takes the
// name of a finished one continues that tree, so short-lived threads ("atm-conn") add up in one place.
void set_thread_name(const char *name);

void set_enabled(bool on);
bool enabled() noexcept;

void begin_frame();
void end_frame();

void reset(); // zero all statistics; zone trees stay

// {"compiled", "enabled", "threads":[{"thread", "alive", "frames", "frame_avg_ms", "frame_peak_ms", "busy_ms",
//   "zones":[{"name", "depth", "calls", "total_ms", "mean_us", "min_us", "max_us", "avg_ms", "peak_ms",
//             "calls_per_frame", "pct"}]}]}   zones are in tree order, slowest child first.
nlohmann::json snapshot();

// Text table of a snapshot() value.
std::string format_report(const nlohmann::json &snapshot);

namespace detail {

struct ThreadState;
extern std::atomic<bool> g_enabled;

ThreadState *enter(const char *name, int &zone) noexcept; // nullptr when the zone is not recorded
void leave(ThreadState *ts, int zone, uint64_t start) noexcept;

inline uint64_t now() noexcept {
#if ATM_PROF_TSC
  return __rdtsc();
#else
  return uint64_t(std::chrono::steady_clock::now().time_since_epoch().count());
#endif
}

} // namespace detail

class Scope {
public:
  explicit Scope(const char *name) noexcept {
    if (detail::g_enabled.load(std::memory_order_relaxed)) {
      ts_ = detail::enter(name, zone_);
      if (ts_)
        start_ = detail::now();
    }
  }
  ~Scope() {
    if (ts_)
      detail::leave(ts_, zone_, start_);
  }
  Scope(const Scope &) = delete;
  Scope &operator=(const Scope &) = delete;

private:
  detail::ThreadState *ts_ = nullptr;
  int zone_ = -1;
  uint64_t start_ = 0;
};

class Frame {
public:
  Frame() { begin_frame(); }
  ~Frame() { end_frame(); }
  Frame(const Frame &) = delete;
  Frame &operator=(const Frame &) = delete;
};

} // namespace atm::prof

#define ATM_PROF_CAT2(a, b) a##b
#define ATM_PROF_CAT(a, b) ATM_PROF_CAT2(a, b)
#if ATM_PROFILING
#define ATM_PROFILE_SCOPE(name) ::atm::prof::Scope ATM_PROF_CAT(atm_prof_scope_, __LINE__)(name)
#define ATM_PROFILE_FRAME() ::atm::prof::Frame ATM_PROF_CAT(atm_prof_frame_, __LINE__)
#else
#define ATM_PROFILE_SCOPE(name) ((void)0)
#define ATM_PROFILE_FRAME() ((void)0)
#endif
