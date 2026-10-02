#include "atm/base/parallel.hpp"

#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "atm/base/profiler.hpp"

namespace atm {
namespace {

struct Job {
  const std::function<void(int64_t, int64_t)> *fn = nullptr;
  int64_t count = 0, chunk = 1, chunks = 0;
  std::atomic<int64_t> next{0}, done{0};
  int users = 0; // workers that hold a pointer to this job; guarded by the pool mutex
};

class Pool {
public:
  Pool() {
    const int n = int(std::clamp<unsigned>(std::thread::hardware_concurrency(), 1u, 16u)) - 1;
    for (int i = 0; i < n; ++i)
      threads_.emplace_back([this, i] { work(i); });
  }
  ~Pool() {
    {
      std::lock_guard lock(mutex_);
      stop_ = true;
    }
    work_cv_.notify_all();
    for (auto &t : threads_)
      t.join();
  }
  int size() const { return int(threads_.size()) + 1; }

  void run(Job &job) {
    if (!threads_.empty()) {
      {
        std::lock_guard lock(mutex_);
        jobs_.push_back(&job);
      }
      work_cv_.notify_all();
    }
    drain(job); // the caller works too
    std::unique_lock lock(mutex_);
    std::erase(jobs_, &job);
    done_cv_.wait(lock, [&] { return job.done.load() == job.chunks && job.users == 0; });
  }

private:
  static void drain(Job &job) {
    for (;;) {
      const int64_t c = job.next.fetch_add(1);
      if (c >= job.chunks)
        return;
      const int64_t begin = c * job.chunk;
      (*job.fn)(begin, std::min(job.count, begin + job.chunk));
      job.done.fetch_add(1);
    }
  }

  void work(int index) {
    const std::string name = "atm-pool-" + std::to_string(index);
    prof::set_thread_name(name.c_str());
    std::unique_lock lock(mutex_);
    for (;;) {
      work_cv_.wait(lock, [&] { return stop_ || !jobs_.empty(); });
      if (stop_)
        return;
      Job *job = jobs_.front();
      ++job->users;
      lock.unlock();
      drain(*job);
      lock.lock();
      std::erase(jobs_, job); // nothing left to take from it
      --job->users;
      done_cv_.notify_all();
    }
  }

  std::mutex mutex_;
  std::condition_variable work_cv_, done_cv_;
  std::vector<Job *> jobs_;
  bool stop_ = false;
  std::vector<std::thread> threads_;
};

Pool &pool() {
  static Pool p;
  return p;
}

} // namespace

int worker_count() { return pool().size(); }

void parallel_for(int64_t count, int64_t min_chunk, const std::function<void(int64_t, int64_t)> &fn) {
  if (count <= 0)
    return;
  Pool &p = pool();
  // About four chunks per thread evens out uneven rows without much scheduling cost.
  const int64_t chunk = std::max<int64_t>(std::max<int64_t>(1, min_chunk), (count + p.size() * 4 - 1) / (p.size() * 4));
  if (chunk >= count) {
    fn(0, count);
    return;
  }
  Job job;
  job.fn = &fn;
  job.count = count;
  job.chunk = chunk;
  job.chunks = (count + chunk - 1) / chunk;
  p.run(job);
}

} // namespace atm
