// Copyright Codevedas Inc. 2025-present
//
// This source code is licensed under the MIT license found in the
// LICENSE file in the root directory of this source tree.

#ifndef VAJRA_RUNTIME_WORKER_POOL_HPP
#define VAJRA_RUNTIME_WORKER_POOL_HPP

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <sys/types.h>
#include <utility>
#include <vector>

namespace Vajra
{
  namespace runtime
  {
    enum class WorkerLifecycleState : std::uint8_t
    {
      booting = 1,
      ready = 2,
      stopping = 3,
      exited = 4,
    };

    enum class WorkerExitClassification : std::uint8_t
    {
      none = 0,
      expected_shutdown = 1,
      exit_before_ready = 2,
      unexpected_status = 3,
      unexpected_signal = 4,
      unexpected_exit = 5,
    };

    struct SharedWorkerState
    {
      SharedWorkerState(
          std::size_t index,
          pid_t worker_pid,
          std::vector<int> parent_request_channels)
          : worker_index(index),
            request_channel_fds(std::move(parent_request_channels)),
            pid(worker_pid)
      {
      }

      const std::size_t worker_index;
      const std::vector<int> request_channel_fds;
      std::atomic<pid_t> pid;
      std::atomic<WorkerLifecycleState> lifecycle_state{WorkerLifecycleState::booting};
      std::atomic_bool available{false};
      std::atomic<WorkerExitClassification> last_exit_classification{WorkerExitClassification::none};
      std::atomic<int> last_exit_detail{0};
      std::atomic_bool replacement_needed{false};
      std::atomic_bool expected_shutdown{false};
      std::atomic_bool timeout_escalation_pending{false};
      std::atomic<std::int64_t> timeout_kill_deadline_nanoseconds{0};
      std::atomic_bool request_channels_closed{false};
      std::atomic_bool timeout_handling_started{false};
    };
  }
}

#endif
