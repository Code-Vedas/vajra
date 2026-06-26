// Copyright Codevedas Inc. 2025-present
//
// This source code is licensed under the MIT license found in the
// LICENSE file in the root directory of this source tree.

#ifndef VAJRA_RACK_RACK_EXECUTION_PROFILER_HPP
#define VAJRA_RACK_RACK_EXECUTION_PROFILER_HPP

#include <atomic>
#include <cstdint>

namespace Vajra
{
  namespace rack
  {
    struct RuntimeProfilingCounters
    {
      std::atomic<std::uint64_t> scheduler_selection_count{0};
      std::atomic<std::int64_t> scheduler_selection_nanoseconds{0};
      std::atomic<std::uint64_t> ruby_execution_count{0};
      std::atomic<std::int64_t> ruby_execution_nanoseconds{0};
      std::atomic<std::uint64_t> response_completion_count{0};
      std::atomic<std::int64_t> response_completion_nanoseconds{0};
    };

    RuntimeProfilingCounters &runtime_profiling_counters();

    class ScopedProfilingSample
    {
    public:
      ScopedProfilingSample(std::atomic<std::uint64_t> &count, std::atomic<std::int64_t> &nanoseconds);
      ~ScopedProfilingSample();

      ScopedProfilingSample(const ScopedProfilingSample &) = delete;
      ScopedProfilingSample &operator=(const ScopedProfilingSample &) = delete;

    private:
      std::atomic<std::uint64_t> &count_;
      std::atomic<std::int64_t> &nanoseconds_;
      std::int64_t started_at_;
    };
  }
}

#endif
