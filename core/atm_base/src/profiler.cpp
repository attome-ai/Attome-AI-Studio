#include "atm/base/profiler.hpp"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <thread>
#include <vector>

#include <nlohmann/json.hpp>

#if defined(_WIN32)
#include <windows.h>
#else
#include <pthread.h>
#endif

namespace atm::prof {
namespace detail {

using Clock = std::chrono::steady_clock;

struct Zone {
  // Written once by the owning thread before the zone is published through ThreadState::count.
  const char *name = nullptr;
  int parent = -1;
  // Owner-only.
  int first_child = -1;
  int next_sibling = -1;
  uint64_t frame_ticks = 0;
  uint32_t frame_calls = 0;
  uint64_t peak_window = 0;
  // Written by the owner, read by snapshot().
  std::atomic<uint64_t> calls{0};
  std::atomic<uint64_t> total{0};
  std::atomic<uint64_t> min{UINT64_MAX};
  std::atomic<uint64_t> max{0};
  std::atomic<uint64_t> avg{0};  // smoothed ticks per frame
  std::atomic<uint64_t> peak{0}; // max ticks in one frame over the last second
  std::atomic<uint32_t> calls_per_frame_milli{0};
};

struct ThreadState {
  char name[32] = {};
  std::atomic<bool> alive{false};
  std::atomic<int> count{0};
  int root_first = -1;
  int depth = 0;
  int stack[kMaxDepth] = {};
  bool in_frame = false;
  uint64_t frame_start = 0;
  uint64_t frame_peak_window = 0;
  Clock::time_point window_start{};
  uint32_t seen_reset = 0;
  std::atomic<uint64_t> frames{0};
  std::atomic<uint64_t> busy{0}; // ticks inside frames
  std::atomic<uint64_t> frame_avg{0};
  std::atomic<uint64_t> frame_peak{0};
  Zone zones[kMaxZones];
};

std::atomic<bool> g_enabled{true};

namespace {

constexpr auto relaxed = std::memory_order_relaxed;

struct Registry {
  std::mutex mutex;
  std::vector<ThreadState *> states; // never freed: readers may hold them at any time
};

Registry &registry() {
  static Registry *r = new Registry; // leaked on purpose; threads may outlive static destruction
  return *r;
}

std::atomic<uint32_t> g_reset{0};

struct Guard {
  ThreadState *ts = nullptr;
  ~Guard() {
    if (ts)
      ts->alive.store(false);
  }
};
thread_local Guard t_guard;

ThreadState *attach(const char *name) {
  Registry &r = registry();
  std::lock_guard lock(r.mutex);
  ThreadState *ts = nullptr;
  if (name)
    for (ThreadState *s : r.states)
      if (!s->alive.load() && std::strcmp(s->name, name) == 0) {
        ts = s;
        break;
      }
  if (!ts) {
    ts = new ThreadState;
    if (name)
      std::snprintf(ts->name, sizeof ts->name, "%s", name);
    else
      std::snprintf(ts->name, sizeof ts->name, "thread-%zu", r.states.size());
    ts->seen_reset = g_reset.load(relaxed);
    r.states.push_back(ts);
  }
  ts->depth = 0;
  ts->in_frame = false;
  ts->alive.store(true);
  t_guard.ts = ts;
  return ts;
}

void apply_reset(ThreadState *ts) {
  ts->seen_reset = g_reset.load(relaxed);
  const int n = ts->count.load(relaxed);
  for (int i = 0; i < n; ++i) {
    Zone &z = ts->zones[i];
    z.frame_ticks = 0;
    z.frame_calls = 0;
    z.peak_window = 0;
    z.calls.store(0, relaxed);
    z.total.store(0, relaxed);
    z.min.store(UINT64_MAX, relaxed);
    z.max.store(0, relaxed);
    z.avg.store(0, relaxed);
    z.peak.store(0, relaxed);
    z.calls_per_frame_milli.store(0, relaxed);
  }
  ts->frame_peak_window = 0;
  ts->frames.store(0, relaxed);
  ts->busy.store(0, relaxed);
  ts->frame_avg.store(0, relaxed);
  ts->frame_peak.store(0, relaxed);
}

// Tick length. The timestamp counter is calibrated against the steady clock between process start and the first
// report, so the hot path never pays for it.
struct Calibration {
  uint64_t ticks = now();
  Clock::time_point time = Clock::now();
};
[[maybe_unused]] const Calibration g_start;

[[maybe_unused]] double ns_per_tick() {
#if ATM_PROF_TSC
  static std::mutex m;
  static double cached = 0.0;
  static double cached_span_ns = 0.0;
  std::lock_guard lock(m);
  if (cached_span_ns >= 5.0e7) // 50 ms of baseline is accurate enough; stop refining
    return cached;
  for (;;) {
    const double ns = double(std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now() - g_start.time).count());
    const uint64_t ticks = now() - g_start.ticks;
    if (ns >= 2.0e6 && ticks > 0) {
      cached = ns / double(ticks);
      cached_span_ns = ns;
      return cached;
    }
    std::this_thread::yield(); // the process is under 2 ms old
  }
#else
  using Period = Clock::period;
  return 1.0e9 * double(Period::num) / double(Period::den);
#endif
}

} // namespace

ThreadState *enter(const char *name, int &zone) noexcept {
  ThreadState *ts = t_guard.ts;
  if (!ts)
    ts = attach(nullptr);
  if (ts->seen_reset != g_reset.load(relaxed))
    apply_reset(ts);
  if (ts->depth >= kMaxDepth)
    return nullptr;
  const int parent = ts->depth ? ts->stack[ts->depth - 1] : -1;
  int &head = parent < 0 ? ts->root_first : ts->zones[parent].first_child;
  int id = head;
  while (id >= 0) {
    const Zone &z = ts->zones[id];
    if (z.name == name || std::strcmp(z.name, name) == 0) // literals are usually the same pointer
      break;
    id = z.next_sibling;
  }
  if (id < 0) {
    const int n = ts->count.load(relaxed);
    if (n >= kMaxZones)
      return nullptr;
    Zone &z = ts->zones[n];
    z.name = name;
    z.parent = parent;
    z.next_sibling = head;
    head = n;
    ts->count.store(n + 1, std::memory_order_release);
    id = n;
  }
  ts->stack[ts->depth++] = id;
  zone = id;
  return ts;
}

void leave(ThreadState *ts, int zone, uint64_t start) noexcept {
  const uint64_t dt = now() - start;
  Zone &z = ts->zones[zone];
  z.frame_ticks += dt;
  ++z.frame_calls;
  z.calls.store(z.calls.load(relaxed) + 1, relaxed);
  z.total.store(z.total.load(relaxed) + dt, relaxed);
  if (dt < z.min.load(relaxed))
    z.min.store(dt, relaxed);
  if (dt > z.max.load(relaxed))
    z.max.store(dt, relaxed);
  if (ts->depth > 0)
    --ts->depth;
}

} // namespace detail

using detail::relaxed;
using detail::ThreadState;
using detail::Zone;

void set_thread_name(const char *name) {
#if defined(_WIN32)
  wchar_t wide[64];
  size_t i = 0;
  for (; name[i] && i + 1 < std::size(wide); ++i)
    wide[i] = wchar_t(static_cast<unsigned char>(name[i]));
  wide[i] = 0;
  SetThreadDescription(GetCurrentThread(), wide);
#elif defined(__APPLE__)
  pthread_setname_np(name);
#else
  char shortened[16]; // Linux limit
  std::snprintf(shortened, sizeof shortened, "%s", name);
  pthread_setname_np(pthread_self(), shortened);
#endif
#if ATM_PROFILING
  if (ThreadState *ts = detail::t_guard.ts) {
    std::lock_guard lock(detail::registry().mutex);
    std::snprintf(ts->name, sizeof ts->name, "%s", name);
  } else {
    detail::attach(name);
  }
#endif
}

void set_enabled(bool on) { detail::g_enabled.store(on, relaxed); }
bool enabled() noexcept { return detail::g_enabled.load(relaxed); }

void reset() { detail::g_reset.fetch_add(1, relaxed); }

void begin_frame() {
  if (!detail::g_enabled.load(relaxed))
    return;
  ThreadState *ts = detail::t_guard.ts;
  if (!ts)
    ts = detail::attach(nullptr);
  if (ts->seen_reset != detail::g_reset.load(relaxed))
    detail::apply_reset(ts);
  ts->in_frame = true;
  ts->frame_start = detail::now();
}

void end_frame() {
  ThreadState *ts = detail::t_guard.ts;
  if (!ts || !ts->in_frame)
    return;
  ts->in_frame = false;
  const uint64_t dt = detail::now() - ts->frame_start;
  const uint64_t frames = ts->frames.load(relaxed);
  ts->frames.store(frames + 1, relaxed);
  ts->busy.store(ts->busy.load(relaxed) + dt, relaxed);
  ts->frame_avg.store(frames == 0 ? dt : (ts->frame_avg.load(relaxed) * 95 + dt * 5) / 100, relaxed);
  ts->frame_peak_window = std::max(ts->frame_peak_window, dt);
  const auto t = detail::Clock::now();
  const bool new_window = t - ts->window_start >= std::chrono::seconds(1);
  if (new_window) {
    ts->window_start = t;
    ts->frame_peak.store(ts->frame_peak_window, relaxed);
    ts->frame_peak_window = 0;
  }
  const int n = ts->count.load(relaxed);
  for (int i = 0; i < n; ++i) {
    Zone &z = ts->zones[i];
    z.avg.store((z.avg.load(relaxed) * 95 + z.frame_ticks * 5) / 100, relaxed);
    z.calls_per_frame_milli.store((z.calls_per_frame_milli.load(relaxed) * 95 + z.frame_calls * 1000 * 5) / 100,
                                  relaxed);
    z.peak_window = std::max(z.peak_window, z.frame_ticks);
    if (new_window) {
      z.peak.store(z.peak_window, relaxed);
      z.peak_window = 0;
    }
    z.frame_ticks = 0;
    z.frame_calls = 0;
  }
}

nlohmann::json snapshot() {
  using nlohmann::json;
  json threads = json::array();
#if ATM_PROFILING
  const double npt = detail::ns_per_tick();
  auto ms = [npt](uint64_t ticks) { return double(ticks) * npt / 1.0e6; };
  auto us = [npt](uint64_t ticks) { return double(ticks) * npt / 1.0e3; };

  detail::Registry &r = detail::registry();
  std::lock_guard lock(r.mutex);
  for (const ThreadState *ts : r.states) {
    const int n = ts->count.load(std::memory_order_acquire);
    std::vector<std::vector<int>> kids(size_t(n) + 1);
    std::vector<uint64_t> total(size_t(n), 0);
    uint64_t roots_total = 0;
    for (int i = 0; i < n; ++i) {
      total[size_t(i)] = ts->zones[i].total.load(relaxed);
      kids[size_t(ts->zones[i].parent + 1)].push_back(i);
      if (ts->zones[i].parent < 0)
        roots_total += total[size_t(i)];
    }
    for (auto &k : kids)
      std::sort(k.begin(), k.end(), [&](int a, int b) { return total[size_t(a)] > total[size_t(b)]; });

    const uint64_t frames = ts->frames.load(relaxed);
    const uint64_t busy = ts->busy.load(relaxed);
    const double whole = double(frames > 0 && busy > 0 ? busy : roots_total);

    json zones = json::array();
    auto emit = [&](auto &&self, int slot, int depth) -> void {
      for (int id : kids[size_t(slot)]) {
        const Zone &z = ts->zones[id];
        const uint64_t calls = z.calls.load(relaxed);
        if (calls > 0) {
          const uint64_t tot = total[size_t(id)];
          zones.push_back({{"name", z.name},
                           {"depth", depth},
                           {"calls", calls},
                           {"total_ms", ms(tot)},
                           {"mean_us", us(tot) / double(calls)},
                           {"min_us", us(z.min.load(relaxed))},
                           {"max_us", us(z.max.load(relaxed))},
                           {"avg_ms", ms(z.avg.load(relaxed))},
                           {"peak_ms", ms(z.peak.load(relaxed))},
                           {"calls_per_frame", double(z.calls_per_frame_milli.load(relaxed)) / 1000.0},
                           {"pct", whole > 0.0 ? double(tot) / whole * 100.0 : 0.0}});
        }
        self(self, id + 1, depth + 1);
      }
    };
    emit(emit, 0, 0);
    if (zones.empty() && frames == 0)
      continue;
    threads.push_back({{"thread", ts->name},
                       {"alive", ts->alive.load()},
                       {"frames", frames},
                       {"frame_avg_ms", ms(ts->frame_avg.load(relaxed))},
                       {"frame_peak_ms", ms(ts->frame_peak.load(relaxed))},
                       {"busy_ms", ms(busy)},
                       {"zones", std::move(zones)}});
  }
#endif
  return {{"compiled", ATM_PROFILING != 0}, {"enabled", enabled()}, {"threads", std::move(threads)}};
}

std::string format_report(const nlohmann::json &snap) {
  std::string out;
  char line[256];
  if (!snap.value("compiled", false))
    return "The profiler is compiled out (ATTOME_PROFILING=OFF).\n";
  if (!snap.value("enabled", false))
    out += "The profiler is switched off; the numbers below are not being updated.\n";
  const auto &threads = snap["threads"];
  if (threads.empty())
    return out + "No zones recorded yet.\n";
  for (const auto &t : threads) {
    std::snprintf(line, sizeof line, "[%s] %llu frames | avg %.3f ms | 1 s peak %.3f ms | busy %.1f ms\n",
                  t.value("thread", "?").c_str(), static_cast<unsigned long long>(t.value("frames", uint64_t(0))),
                  t.value("frame_avg_ms", 0.0), t.value("frame_peak_ms", 0.0), t.value("busy_ms", 0.0));
    out += line;
    std::snprintf(line, sizeof line, "  %-34s %9s %11s %10s %10s %10s %6s\n", "zone", "calls", "total ms", "mean us",
                  "min us", "max us", "%");
    out += line;
    for (const auto &z : t["zones"]) {
      const std::string name = std::string(size_t(z.value("depth", 0)) * 2, ' ') + z.value("name", "?");
      std::snprintf(line, sizeof line, "  %-34.34s %9llu %11.3f %10.2f %10.2f %10.2f %6.1f\n", name.c_str(),
                    static_cast<unsigned long long>(z.value("calls", uint64_t(0))), z.value("total_ms", 0.0),
                    z.value("mean_us", 0.0), z.value("min_us", 0.0), z.value("max_us", 0.0), z.value("pct", 0.0));
      out += line;
    }
  }
  return out;
}

} // namespace atm::prof
