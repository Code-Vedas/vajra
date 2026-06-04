// Copyright Codevedas Inc. 2025-present
//
// This source code is licensed under the MIT license found in the
// LICENSE file in the root directory of this source tree.

#ifndef VAJRA_RUNTIME_WORKER_POOL_HPP
#define VAJRA_RUNTIME_WORKER_POOL_HPP

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <string>
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

    enum class WorkerHealthState : std::uint8_t
    {
      healthy = 1,
      busy = 2,
      overloaded = 3,
      degraded = 4,
      suspect = 5,
      wedged = 6,
    };

    enum class WorkerRecoveryState : std::uint8_t
    {
      none = 0,
      draining = 1,
      terminating = 2,
      replacing = 3,
      rejoin_pending = 4,
      terminal_failure = 5,
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
      mutable std::mutex request_channel_mutex;
      std::vector<int> request_channel_fds;
      std::atomic<pid_t> pid;
      std::atomic<WorkerLifecycleState> lifecycle_state{WorkerLifecycleState::booting};
      std::atomic<WorkerHealthState> health_state{WorkerHealthState::healthy};
      std::atomic_bool available{false};
      std::atomic<WorkerExitClassification> last_exit_classification{WorkerExitClassification::none};
      std::atomic<int> last_exit_detail{0};
      std::atomic_bool replacement_needed{false};
      std::atomic_bool expected_shutdown{false};
      std::atomic_bool timeout_escalation_pending{false};
      std::atomic<std::int64_t> timeout_kill_deadline_nanoseconds{0};
      std::atomic_bool request_channels_closed{false};
      std::atomic_bool timeout_handling_started{false};
      std::atomic<std::uint64_t> channel_generation{0};
      std::atomic<std::size_t> active_execution_count{0};
      std::atomic<std::size_t> idle_execution_count{0};
      std::atomic<std::size_t> local_queue_depth{0};
      std::atomic<std::int64_t> oldest_local_queue_age_nanoseconds{0};
      std::atomic<std::int64_t> last_progress_nanoseconds{0};
      std::atomic<std::int64_t> last_lifecycle_transition_nanoseconds{0};
      std::atomic<std::int64_t> last_health_transition_nanoseconds{0};
      std::atomic<std::uint64_t> health_transition_count{0};
      std::atomic<std::uint64_t> timeout_escalation_count{0};
      std::atomic<std::uint64_t> unexpected_exit_count{0};
      std::atomic<std::int64_t> last_unexpected_exit_nanoseconds{0};
      std::atomic<std::uint64_t> replacement_attempt_count{0};
      std::atomic<std::uint64_t> replacement_success_count{0};
      std::atomic<std::uint64_t> replacement_failure_count{0};
      std::atomic<WorkerRecoveryState> recovery_state{WorkerRecoveryState::none};
      std::atomic_bool terminal_replacement_failure{false};
      std::atomic_bool spawned_by_worker_spawner{false};
      std::atomic<std::int64_t> overload_started_nanoseconds{0};
      std::atomic<std::int64_t> recovery_deadline_nanoseconds{0};
      std::atomic_bool recovery_requested{false};
      std::atomic_bool replacement_scheduled{false};
    };
  }
}

#endif
