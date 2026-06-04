// Copyright Codevedas Inc. 2025-present
//
// This source code is licensed under the MIT license found in the
// LICENSE file in the root directory of this source tree.

#ifndef VAJRA_RUNTIME_NATIVE_RUNTIME_HPP
#define VAJRA_RUNTIME_NATIVE_RUNTIME_HPP

#include "runtime/boot_contract.hpp"
#include "runtime/runtime_config.hpp"
#include "runtime/worker_pool.hpp"
#include "server.hpp"

#include <atomic>
#include <condition_variable>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <sys/types.h>
#include <thread>
#include <vector>

namespace Vajra
{
  namespace runtime
  {
    struct HealthPolicy
    {
      std::int64_t overload_oldest_queue_age_nanoseconds = 0;
      std::int64_t overload_recovery_threshold_nanoseconds = 0;
      std::int64_t suspect_threshold_nanoseconds = 0;
      std::int64_t wedged_threshold_nanoseconds = 0;
      std::int64_t degraded_decay_nanoseconds = 0;
      std::int64_t drain_deadline_nanoseconds = 0;
    };

    struct WorkerSpawnConfig
    {
      std::size_t max_threads = 0;
      int port = 0;
      std::size_t max_request_head_bytes = 0;
      int worker_processes = 0;
      bool debug_logging = false;
    };

    struct RecoveryPolicy
    {
      std::uint64_t replacement_failure_limit = 3;
    };

    class NativeRuntime final
    {
    public:
      static NativeRuntime &instance();
      static bool shutdown_requested();

      void begin_runtime_shutdown();
      void forward_shutdown_to_workers();
      void start(const RuntimeConfig &config);
      void stop();
      std::vector<std::shared_ptr<SharedWorkerState>> worker_states() const;

    private:
      NativeRuntime() = default;

      bool runtime_running() const;
      bool try_begin_startup();
      bool stop_worker_processes();
      void replay_pending_stop_if_needed();
      std::shared_ptr<SharedWorkerState> register_worker_runtime(
          std::size_t worker_index,
          pid_t pid,
          std::vector<int> request_channel_fds);
      void mark_worker_ready(const std::shared_ptr<SharedWorkerState> &worker_state);
      void mark_worker_stopping(const std::shared_ptr<SharedWorkerState> &worker_state);
      void mark_worker_exit(
          const std::shared_ptr<SharedWorkerState> &worker_state,
          WorkerExitClassification exit_classification,
          int exit_detail);
      void prepare_worker_replacement(const std::shared_ptr<SharedWorkerState> &worker_state);
      void close_worker_request_channels(const std::shared_ptr<SharedWorkerState> &worker_state);
      void replace_failed_workers(const std::vector<std::shared_ptr<SharedWorkerState>> &worker_states);
      void maybe_recover_unhealthy_workers(const std::vector<std::shared_ptr<SharedWorkerState>> &worker_states);
      void initiate_worker_recovery(const std::shared_ptr<SharedWorkerState> &worker_state);
      void drain_pending_replacements();
      void replace_worker(const std::shared_ptr<SharedWorkerState> &worker_state);
      bool start_worker_spawner(const WorkerSpawnConfig &spawn_config);
      void stop_worker_spawner();
      [[noreturn]] void run_worker_spawner(int control_fd, const WorkerSpawnConfig &spawn_config);
      bool spawn_worker_from_single_thread(
          std::size_t worker_index,
          const WorkerSpawnConfig &spawn_config,
          pid_t &pid,
          std::vector<int> &parent_request_channels,
          BootDiagnostic &failure_diagnostic,
          int inherited_control_fd);
      bool boot_replacement_worker(
          const std::shared_ptr<SharedWorkerState> &worker_state,
          const WorkerSpawnConfig &spawn_config,
          pid_t &pid,
          std::vector<int> &parent_request_channels,
          BootDiagnostic &failure_diagnostic);
      void clear_worker_runtime();
      void install_server_instance(std::shared_ptr<Vajra::Server> server);
      std::shared_ptr<Vajra::Server> take_server_instance();
      void wait_for_worker_exit(const std::vector<std::shared_ptr<SharedWorkerState>> &worker_states);
      void wait_for_worker_exit_blocking(const std::vector<std::shared_ptr<SharedWorkerState>> &worker_states);
      static void *wait_for_worker_exit_without_gvl(void *data);
      void observe_worker_exit(
          const std::shared_ptr<SharedWorkerState> &worker_state,
          int status);
      void observe_worker_disappearance(const std::shared_ptr<SharedWorkerState> &worker_state);
      void refresh_worker_health(const std::vector<std::shared_ptr<SharedWorkerState>> &worker_states);
      void handle_worker_timeout(const std::shared_ptr<SharedWorkerState> &worker_state);
      void maybe_escalate_timed_out_workers(const std::vector<std::shared_ptr<SharedWorkerState>> &worker_states);
      void ensure_worker_exit_watcher_started();
      void stop_worker_exit_watcher();
      void watch_worker_exits();
      void run_worker_process(
          std::vector<int> request_channel_fds,
          std::size_t max_threads,
          int port,
          std::size_t max_request_head_bytes,
          int readiness_write_fd,
          int worker_index,
          int worker_processes,
          bool debug_logging);
      void run_master_runtime_server(
          const RuntimeConfig &config,
          const std::vector<std::shared_ptr<SharedWorkerState>> &worker_states,
          bool debug_logging);

      bool runtime_shutdown_started_ = false;
      mutable std::mutex server_mutex_;
      std::condition_variable worker_state_changed_;
      std::shared_ptr<Vajra::Server> server_instance_;
      std::vector<std::shared_ptr<SharedWorkerState>> worker_states_;
      bool stop_requested_ = false;
      bool worker_startup_in_progress_ = false;
      HealthPolicy health_policy_{};
      WorkerSpawnConfig worker_spawn_config_{};
      RecoveryPolicy recovery_policy_{};
      std::atomic_bool debug_logging_{false};
      std::vector<std::shared_ptr<SharedWorkerState>> pending_replacements_;
      pid_t worker_spawner_pid_ = -1;
      int worker_spawner_fd_ = -1;
      bool worker_exit_watcher_stop_requested_ = false;
      bool worker_exit_watcher_running_ = false;
      std::thread worker_exit_watcher_;
    };
  }
}

#endif
