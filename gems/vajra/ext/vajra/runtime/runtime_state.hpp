// Copyright Codevedas Inc. 2025-present
//
// This source code is licensed under the MIT license found in the
// LICENSE file in the root directory of this source tree.

#ifndef VAJRA_RUNTIME_RUNTIME_STATE_HPP
#define VAJRA_RUNTIME_RUNTIME_STATE_HPP

#include "runtime/worker_pool.hpp"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <string>
#include <sys/types.h>

namespace Vajra
{
  namespace runtime
  {
    constexpr std::size_t kMaxTrackedWorkers = 1'024;

    struct WorkerRuntimeState
    {
      std::atomic<pid_t> pid{0};
      std::atomic<std::uint8_t> lifecycle_state{static_cast<std::uint8_t>(WorkerLifecycleState::booting)};
      std::atomic<std::uint8_t> health_state{static_cast<std::uint8_t>(WorkerHealthState::healthy)};
      std::atomic<std::uint8_t> recovery_state{static_cast<std::uint8_t>(WorkerRecoveryState::none)};
      std::atomic_bool available{false};
      std::atomic<std::size_t> active_connections{0};
      std::atomic<std::size_t> active_execution_count{0};
      std::atomic<std::size_t> idle_execution_count{0};
      std::atomic<std::size_t> local_queue_depth{0};
      std::atomic<std::uint64_t> accept_count{0};
      std::atomic<std::uint64_t> dispatch_count{0};
      std::atomic<std::uint64_t> receive_count{0};
      std::atomic<std::uint64_t> fd_transfer_failures{0};
      std::atomic<std::uint64_t> completed_request_count{0};
      std::atomic<std::int64_t> request_head_nanoseconds{0};
      std::atomic<std::int64_t> request_parse_nanoseconds{0};
      std::atomic<std::int64_t> request_body_nanoseconds{0};
      std::atomic<std::int64_t> request_total_nanoseconds{0};
      std::atomic<std::int64_t> rack_start_nanoseconds{0};
      std::atomic<std::int64_t> rack_finish_nanoseconds{0};
      std::atomic<std::int64_t> rack_execution_nanoseconds{0};
      std::atomic<std::int64_t> response_write_nanoseconds{0};
      std::atomic<std::int64_t> last_progress_nanoseconds{0};
      std::atomic<std::uint64_t> replacement_attempt_count{0};
      std::atomic<std::uint64_t> replacement_success_count{0};
      std::atomic<std::uint64_t> replacement_failure_count{0};
      std::atomic<std::uint64_t> timeout_escalation_count{0};
      std::atomic<std::uint64_t> unexpected_exit_count{0};
      std::atomic<std::int64_t> last_unexpected_exit_nanoseconds{0};
      std::atomic<std::int64_t> overload_started_nanoseconds{0};
      std::atomic<std::int64_t> recovery_deadline_nanoseconds{0};
      std::atomic_bool terminal_replacement_failure{false};
    };

    struct RuntimeState
    {
      std::atomic<pid_t> master_pid{0};
      std::atomic<int> listener_fd{-1};
      std::atomic<std::uint32_t> worker_count{0};
      std::atomic<std::uint32_t> threads_per_worker{0};
      std::atomic<std::uint32_t> socket_queue_capacity{0};
      std::atomic_bool shutdown_requested{false};
      WorkerRuntimeState workers[kMaxTrackedWorkers];
    };

    RuntimeState *allocate_runtime_state();
    void release_runtime_state(RuntimeState *state);

    void install_master_runtime_state(
        RuntimeState *state,
        std::size_t worker_count,
        std::size_t threads_per_worker,
        std::size_t socket_queue_capacity);
    void install_worker_runtime_state(RuntimeState *state, std::size_t worker_index, pid_t pid);
    void attach_current_thread_to_worker_runtime_state(std::size_t worker_index);
    void detach_worker_runtime_state();

    RuntimeState *current_runtime_state();
    WorkerRuntimeState *current_worker_runtime_state();

    void set_runtime_listener_fd(int listener_fd);
    int runtime_listener_fd();
    void mark_runtime_shutdown_requested();

    void mark_worker_lifecycle(std::size_t worker_index, WorkerLifecycleState lifecycle_state);
    void mark_worker_recovery(std::size_t worker_index, WorkerRecoveryState recovery_state);
    void mark_worker_health(std::size_t worker_index, WorkerHealthState health_state);
    void mark_worker_available(std::size_t worker_index, bool available);
    void mark_worker_replacement_counters(
        std::size_t worker_index,
        std::uint64_t attempt_count,
        std::uint64_t success_count,
        std::uint64_t failure_count);
    void mark_worker_timeout_escalations(std::size_t worker_index, std::uint64_t count);
    void mark_worker_unexpected_exit(std::size_t worker_index, std::uint64_t count, std::int64_t last_exit_nanoseconds);
    void mark_worker_terminal_replacement_failure(std::size_t worker_index, bool terminal_failure);

    void note_worker_connection_opened();
    void note_worker_connection_closed();
    void note_worker_accept();
    void note_worker_dispatch_received();
    void note_worker_request_head_time(std::int64_t nanoseconds);
    void note_worker_request_parse_time(std::int64_t nanoseconds);
    void note_worker_request_body_time(std::int64_t nanoseconds);
    void note_worker_request_time(std::int64_t nanoseconds);
    void note_worker_rack_start_time(std::int64_t nanoseconds);
    void note_worker_rack_finish_time(std::int64_t nanoseconds);
    void note_worker_rack_execution_time(std::int64_t nanoseconds);
    void note_worker_response_write_time(std::int64_t nanoseconds);
    void note_worker_request_completed();
    void note_worker_execution_started();
    void note_worker_execution_finished();
    void note_worker_local_queue_depth(std::size_t queue_depth);
    void note_master_dispatch(std::size_t worker_index, std::int64_t nanoseconds);
    void note_master_fd_transfer_failure(std::size_t worker_index);

    std::string runtime_stats_payload_json();
    std::string runtime_metrics_payload_text();
  }
}

#endif
