// Copyright Codevedas Inc. 2025-present
//
// This source code is licensed under the MIT license found in the
// LICENSE file in the root directory of this source tree.

#include "rack/rack_execution_profiler.hpp"

#include <chrono>

namespace
{
  std::int64_t steady_clock_nanoseconds_now()
  {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
  }
}

Vajra::rack::RuntimeProfilingCounters &Vajra::rack::runtime_profiling_counters()
{
  static RuntimeProfilingCounters counters;
  return counters;
}

Vajra::rack::ScopedProfilingSample::ScopedProfilingSample(
    std::atomic<std::uint64_t> &count,
    std::atomic<std::int64_t> &nanoseconds)
    : count_(count),
      nanoseconds_(nanoseconds),
      started_at_(steady_clock_nanoseconds_now())
{
}

Vajra::rack::ScopedProfilingSample::~ScopedProfilingSample()
{
  count_.fetch_add(1, std::memory_order_acq_rel);
  nanoseconds_.fetch_add(steady_clock_nanoseconds_now() - started_at_, std::memory_order_acq_rel);
}
