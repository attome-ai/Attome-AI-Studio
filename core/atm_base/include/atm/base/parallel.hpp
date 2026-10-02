#pragma once
// A shared pool of worker threads ("atm-pool-N") for data-parallel loops: pixel rows, planes, tiles.

#include <cstdint>
#include <functional>

namespace atm {

// Runs fn(begin, end) over [0, count) in chunks of at least `min_chunk` items, on the pool and on the calling
// thread, and returns when every chunk is done. Calls from several threads at once share the pool.
void parallel_for(int64_t count, int64_t min_chunk, const std::function<void(int64_t, int64_t)> &fn);

int worker_count(); // threads that run a parallel_for, the caller included

} // namespace atm
