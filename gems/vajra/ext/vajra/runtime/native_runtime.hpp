// Copyright Codevedas Inc. 2025-present
//
// This source code is licensed under the MIT license found in the
// LICENSE file in the root directory of this source tree.

#ifndef VAJRA_RUNTIME_NATIVE_RUNTIME_HPP
#define VAJRA_RUNTIME_NATIVE_RUNTIME_HPP

#include "runtime/runtime_config.hpp"
#include "server.hpp"

#include <memory>
#include <mutex>
#include <string>
#include <sys/types.h>
#include <vector>

namespace Vajra
{
  namespace runtime
  {
    class NativeRuntime final
    {
    public:
      static NativeRuntime &instance();
      static bool shutdown_requested();

      void begin_runtime_shutdown();
      void forward_shutdown_to_workers();
      void start(const RuntimeConfig &config);
      void stop();

    private:
      NativeRuntime() = default;

      bool runtime_running() const;
      bool try_begin_startup();
      bool stop_worker_processes();
      void replay_pending_stop_if_needed();
      void set_worker_runtime(pid_t pid, int request_channel_fd);
      void clear_worker_runtime();
      void install_server_instance(std::shared_ptr<Vajra::Server> server);
      std::shared_ptr<Vajra::Server> take_server_instance();
      void wait_for_worker_exit(const std::vector<pid_t> &pids);
      void reap_worker_process(pid_t pid) const;
      void reap_worker_processes(const std::vector<pid_t> &pids) const;
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
          const std::vector<std::vector<int>> &request_channel_fds,
          const std::vector<pid_t> &worker_pids_for_runtime,
          bool debug_logging);

      bool runtime_shutdown_started_ = false;
      mutable std::mutex server_mutex_;
      std::shared_ptr<Vajra::Server> server_instance_;
      std::vector<pid_t> worker_pids_;
      std::vector<int> worker_request_channel_fds_;
      bool stop_requested_ = false;
      bool worker_startup_in_progress_ = false;
    };
  }
}

#endif
