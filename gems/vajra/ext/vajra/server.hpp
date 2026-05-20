// Copyright Codevedas Inc. 2025-present
//
// This source code is licensed under the MIT license found in the
// LICENSE file in the root directory of this source tree.

#ifndef VAJRA_SERVER_HPP
#define VAJRA_SERVER_HPP

#include <cstddef>
#include <functional>
#include <atomic>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "lifecycle/lifecycle_controller.hpp"
#include "listener/listener_socket.hpp"
#include "request/request_head_error.hpp"
#include "request/request_processor.hpp"

namespace Vajra
{
  class Server
  {
  public:
    explicit Server(
        int port,
        std::string host = "0.0.0.0",
        std::size_t max_request_head_bytes = request::kDefaultMaxRequestHeadBytes,
        std::shared_ptr<const request::RequestExecutor> request_executor = nullptr,
        std::string process_role = "native_runtime_single_process",
        std::string runtime_mode = "single_process",
        int worker_processes = 0,
        std::string request_execution_role = "single_process_bootstrap",
        bool debug_logging = false,
        int inherited_listener_fd = -1,
        int request_head_timeout_seconds = 5,
        int first_data_timeout_seconds = 30,
        int persistent_timeout_seconds = 30,
        std::function<void()> shutdown_begin_callback = {});
    ~Server();

    void start();
    void stop();
    lifecycle::Snapshot lifecycle_snapshot() const;
    void set_lifecycle_observer(lifecycle::Controller::Observer observer);

  private:
    struct HandlerThread
    {
      std::thread thread;
      std::shared_ptr<std::atomic<bool>> completed;
    };

    std::string host_;
    int port_;
    std::atomic<int> server_fd_;
    listener::Socket listener_socket_;
    request::RequestProcessor request_processor_;
    lifecycle::Controller lifecycle_;
    std::string process_role_;
    std::string runtime_mode_;
    int worker_processes_;
    std::string request_execution_role_;
    bool debug_logging_;
    std::function<void()> shutdown_begin_callback_;
    std::mutex handler_threads_mutex_;
    std::vector<HandlerThread> handler_threads_;

    void close_listener_fd(bool interrupt_accept);
    void join_handler_threads();
    void reap_completed_handler_threads();
  };
}

#endif
