// Copyright Codevedas Inc. 2025-present
//
// This source code is licensed under the MIT license found in the
// LICENSE file in the root directory of this source tree.

#ifndef VAJRA_SERVER_HPP
#define VAJRA_SERVER_HPP

#include <cstddef>
#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
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
        int request_body_timeout_seconds = request::kDefaultRequestBodyTimeoutSeconds,
        int persistent_timeout_seconds = 30,
        std::size_t max_connections = 256,
        std::function<void()> shutdown_begin_callback = {},
        std::size_t max_request_body_bytes = request::kDefaultMaxRequestBodyBytes);
    ~Server();

    void start();
    void stop();
    lifecycle::Snapshot lifecycle_snapshot() const;
    void set_lifecycle_observer(lifecycle::Controller::Observer observer);

  private:
    struct ActiveClientRegistration
    {
      int original_fd;
      bool open;
      std::size_t descriptor_count;
    };

    struct HandlerThread
    {
      std::thread thread;
    };

    struct PendingClient
    {
      int fd;
      request::SocketContext socket_context;
      std::uint64_t token;
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
    std::size_t max_connections_;
    std::size_t max_tracked_client_descriptors_;
    std::atomic<std::size_t> active_connection_count_{0};
    std::size_t active_tracked_client_descriptors_ = 0;
    std::function<void()> shutdown_begin_callback_;
    std::mutex handler_threads_mutex_;
    std::vector<HandlerThread> handler_threads_;
    std::mutex connection_queue_mutex_;
    std::condition_variable connection_queue_condition_;
    std::deque<PendingClient> pending_clients_;
    bool connection_workers_stopping_ = false;
    std::mutex active_client_fds_mutex_;
    std::unordered_map<std::uint64_t, ActiveClientRegistration> active_client_fds_;
    std::atomic<std::uint64_t> next_active_client_token_{0};

    void close_listener_fd(bool interrupt_accept);
    void start_handler_threads();
    void join_handler_threads();
    void reap_completed_handler_threads();
    void enqueue_pending_client(PendingClient client);
    void run_handler_thread();
    void handle_pending_client(PendingClient client);
    std::uint64_t register_active_client_fd(int client_fd);
    void unregister_active_client_fd(int client_fd, std::uint64_t client_token);
    void interrupt_active_client_sockets() noexcept;
  };
}

#endif
