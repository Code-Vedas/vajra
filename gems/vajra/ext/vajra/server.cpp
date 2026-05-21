// Copyright Codevedas Inc. 2025-present
//
// This source code is licensed under the MIT license found in the
// LICENSE file in the root directory of this source tree.

#include "server.hpp"
#include "vajra.hpp"

#include <arpa/inet.h>
#include <cerrno>
#include <chrono>
#include <cstring>
#include <cstdint>
#include <ctime>
#include <fcntl.h>
#include <iomanip>
#include <iostream>
#include <poll.h>
#include <sstream>
#include <stdexcept>
#include <sys/socket.h>
#include <unistd.h>
#include <utility>

namespace
{
  constexpr const char *kUnknownSocketAddress = "0.0.0.0";
  constexpr int kHandlerReapPollTimeoutMilliseconds = 1000;

  std::string socket_address(sockaddr_in address)
  {
    char buffer[INET_ADDRSTRLEN] = {0};
    if (inet_ntop(AF_INET, &address.sin_addr, buffer, sizeof(buffer)) == nullptr)
    {
      return kUnknownSocketAddress;
    }

    return buffer;
  }

  Vajra::request::SocketContext socket_context_for(int client_fd, const sockaddr_in &client_addr, int fallback_port)
  {
    sockaddr_in local_addr{};
    socklen_t local_addr_length = sizeof(local_addr);
    if (getsockname(client_fd, reinterpret_cast<sockaddr *>(&local_addr), &local_addr_length) != 0)
    {
      local_addr.sin_family = AF_INET;
      local_addr.sin_addr.s_addr = htonl(INADDR_ANY);
      local_addr.sin_port = htons(static_cast<std::uint16_t>(fallback_port));
    }

    return Vajra::request::SocketContext{
        socket_address(client_addr),
        ntohs(client_addr.sin_port),
        socket_address(local_addr),
        ntohs(local_addr.sin_port),
        "http"};
  }

  const char *state_name(Vajra::lifecycle::State state)
  {
    switch (state)
    {
      case Vajra::lifecycle::State::booting:
        return "booting";
      case Vajra::lifecycle::State::listening:
        return "listening";
      case Vajra::lifecycle::State::serving:
        return "serving";
      case Vajra::lifecycle::State::draining:
        return "draining";
      case Vajra::lifecycle::State::stopped:
        return "stopped";
      case Vajra::lifecycle::State::failed:
        return "failed";
    }

    throw std::logic_error("unknown lifecycle state");
  }

  const char *boot_readiness_name(Vajra::lifecycle::BootReadiness readiness)
  {
    switch (readiness)
    {
      case Vajra::lifecycle::BootReadiness::pending:
        return "pending";
      case Vajra::lifecycle::BootReadiness::ready:
        return "ready";
      case Vajra::lifecycle::BootReadiness::failed:
        return "failed";
    }

    throw std::logic_error("unknown lifecycle boot readiness");
  }

  const char *stop_reason_name(Vajra::lifecycle::StopReason reason)
  {
    switch (reason)
    {
      case Vajra::lifecycle::StopReason::none:
        return "none";
      case Vajra::lifecycle::StopReason::programmatic_stop:
        return "programmatic_stop";
      case Vajra::lifecycle::StopReason::signal_shutdown:
        return "signal_shutdown";
      case Vajra::lifecycle::StopReason::startup_failure:
        return "startup_failure";
      case Vajra::lifecycle::StopReason::listener_failure:
        return "listener_failure";
    }

    throw std::logic_error("unknown lifecycle stop reason");
  }

  std::string utc_timestamp()
  {
    const auto now = std::chrono::system_clock::now();
    const std::time_t now_time = std::chrono::system_clock::to_time_t(now);
    std::tm utc_time{};
    gmtime_r(&now_time, &utc_time);

    std::ostringstream timestamp;
    timestamp << std::put_time(&utc_time, "%Y-%m-%dT%H:%M:%SZ");
    return timestamp.str();
  }

  void log_message(const char *event_type, const std::string &message, std::ostream &stream)
  {
    stream << "[Vajra][" << event_type << "] " << utc_timestamp() << ' ' << message << std::endl;
  }

  std::string lifecycle_details(
      const Vajra::lifecycle::Snapshot &snapshot,
      const std::string &process_role,
      const std::string &runtime_mode,
      int worker_processes,
      const std::string &request_execution_role)
  {
    std::ostringstream message;
    message << "state=" << state_name(snapshot.state)
            << " boot_status=" << boot_readiness_name(snapshot.boot_readiness)
            << " stop_reason=" << stop_reason_name(snapshot.last_stop_reason)
            << " port=" << snapshot.port
            << " listener_owned=" << (snapshot.listener_owned ? "true" : "false")
            << " listener_fd=" << snapshot.listener_fd
            << " mode=" << runtime_mode
            << " process_role=" << process_role
            << " request_execution_role=" << request_execution_role
            << " worker_processes=" << worker_processes;
    return message.str();
  }

  void log_runtime_event(
      const char *event_name,
      const Vajra::lifecycle::Snapshot &snapshot,
      const std::string &process_role,
      const std::string &runtime_mode,
      int worker_processes,
      const std::string &request_execution_role,
      std::ostream &stream)
  {
    std::ostringstream message;
    message << "event=" << event_name << ' '
            << lifecycle_details(snapshot, process_role, runtime_mode, worker_processes, request_execution_role);
    log_message("lifecycle", message.str(), stream);
  }

  void log_booting_event(
      const std::string &process_role,
      const std::string &runtime_mode,
      int worker_processes,
      const std::string &request_execution_role,
      bool debug_logging)
  {
    if (!debug_logging)
    {
      return;
    }

    const Vajra::lifecycle::Snapshot snapshot{
        Vajra::lifecycle::State::booting,
        Vajra::lifecycle::BootReadiness::pending,
        Vajra::lifecycle::StopReason::none,
        false,
        -1,
        -1,
    };
    log_runtime_event("booting", snapshot, process_role, runtime_mode, worker_processes, request_execution_role, std::cout);
  }

  void log_listening_banner(const std::string &host, int port)
  {
    std::cout << "[" << getpid() << "] * Listening on http://" << host << ":" << port << std::endl;
    std::cout << "[" << getpid() << "] Use Ctrl-C to stop" << std::endl;
    std::ostringstream message;
    message << "listening on port " << port;
    log_message("lifecycle", message.str(), std::cout);
  }

  void log_accept_failed(const char *error_message)
  {
    std::ostringstream message;
    message << "accept failed: " << error_message;
    log_message("error", message.str(), std::cerr);
  }

  void log_connection_rejected(std::size_t max_connections)
  {
    std::ostringstream message;
    message << "connection rejected: active connection limit reached (max_connections=" << max_connections << ")";
    log_message("error", message.str(), std::cerr);
  }

  void log_handler_thread_failure(const sockaddr_in &client_addr, int client_fd, const std::string &message)
  {
    std::ostringstream error_message;
    error_message << "handler thread failed for client="
                  << socket_address(client_addr) << ':' << ntohs(client_addr.sin_port)
                  << " client_fd=" << client_fd
                  << " error=" << message;
    log_message("error", error_message.str(), std::cerr);
  }

  class ActiveConnectionGuard
  {
  public:
    explicit ActiveConnectionGuard(std::atomic<std::size_t> &active_connection_count)
        : active_connection_count_(active_connection_count)
    {
    }

    ~ActiveConnectionGuard()
    {
      active_connection_count_.fetch_sub(1, std::memory_order_acq_rel);
    }

  private:
    std::atomic<std::size_t> &active_connection_count_;
  };
}

Vajra::Server::Server(
    int port,
    std::string host,
    std::size_t max_request_head_bytes,
    std::shared_ptr<const request::RequestExecutor> request_executor,
    std::string process_role,
    std::string runtime_mode,
    int worker_processes,
    std::string request_execution_role,
    bool debug_logging,
    int inherited_listener_fd,
    int request_head_timeout_seconds,
    int first_data_timeout_seconds,
    int persistent_timeout_seconds,
    std::size_t max_connections,
    std::function<void()> shutdown_begin_callback)
    : host_(std::move(host)),
      port_(port),
      server_fd_(inherited_listener_fd),
      listener_socket_(),
      request_processor_(
          max_request_head_bytes,
          request_head_timeout_seconds,
          first_data_timeout_seconds,
          persistent_timeout_seconds,
          std::move(request_executor)),
      lifecycle_(),
      process_role_(std::move(process_role)),
      runtime_mode_(std::move(runtime_mode)),
      worker_processes_(worker_processes),
      request_execution_role_(std::move(request_execution_role)),
      debug_logging_(debug_logging),
      max_connections_(max_connections),
      shutdown_begin_callback_(std::move(shutdown_begin_callback))
{
  set_lifecycle_observer([this](lifecycle::HookPoint hook_point, const lifecycle::Snapshot &snapshot) {
    switch (hook_point)
    {
      case lifecycle::HookPoint::boot_complete:
        if (!debug_logging_)
        {
          return;
        }
        log_runtime_event("boot_complete", snapshot, process_role_, runtime_mode_, worker_processes_, request_execution_role_, std::cout);
        return;
      case lifecycle::HookPoint::serving_entered:
        if (!debug_logging_)
        {
          return;
        }
        log_runtime_event("serving_entered", snapshot, process_role_, runtime_mode_, worker_processes_, request_execution_role_, std::cout);
        return;
      case lifecycle::HookPoint::drain_requested:
        if (!debug_logging_)
        {
          return;
        }
        log_runtime_event("drain_requested", snapshot, process_role_, runtime_mode_, worker_processes_, request_execution_role_, std::cout);
        return;
      case lifecycle::HookPoint::stop_completed:
        if (!debug_logging_)
        {
          return;
        }
        log_runtime_event("stop_completed", snapshot, process_role_, runtime_mode_, worker_processes_, request_execution_role_, std::cout);
        return;
      case lifecycle::HookPoint::failed_entered:
        log_runtime_event("failed_entered", snapshot, process_role_, runtime_mode_, worker_processes_, request_execution_role_, std::cerr);
        return;
    }

    throw std::logic_error("unknown lifecycle hook point");
  });
}

Vajra::Server::~Server()
{
  close_listener_fd(false);
  join_handler_threads();
}

void Vajra::Server::close_listener_fd(bool interrupt_accept)
{
  const int listener_fd = server_fd_.exchange(-1);
  if (listener_fd < 0)
  {
    return;
  }

  if (interrupt_accept)
  {
    shutdown(listener_fd, SHUT_RDWR);
  }

  close(listener_fd);
}

void Vajra::Server::join_handler_threads()
{
  std::vector<std::thread> handler_threads;
  {
    std::lock_guard<std::mutex> lock(handler_threads_mutex_);
    handler_threads.reserve(handler_threads_.size());
    for (HandlerThread &handler_thread : handler_threads_)
    {
      handler_threads.push_back(std::move(handler_thread.thread));
    }
    handler_threads_.clear();
  }

  for (std::thread &thread : handler_threads)
  {
    if (thread.joinable())
    {
      thread.join();
    }
  }
}

void Vajra::Server::reap_completed_handler_threads()
{
  std::vector<std::thread> completed_threads;
  {
    std::lock_guard<std::mutex> lock(handler_threads_mutex_);
    auto handler_thread = handler_threads_.begin();
    while (handler_thread != handler_threads_.end())
    {
      if (!handler_thread->completed->load(std::memory_order_acquire))
      {
        ++handler_thread;
        continue;
      }

      completed_threads.push_back(std::move(handler_thread->thread));
      handler_thread = handler_threads_.erase(handler_thread);
    }
  }

  for (std::thread &thread : completed_threads)
  {
    if (thread.joinable())
    {
      thread.join();
    }
  }
}

void Vajra::Server::start()
{
  if (!lifecycle_.begin_startup())
  {
    close_listener_fd(false);
    return;
  }

  log_booting_event(process_role_, runtime_mode_, worker_processes_, request_execution_role_, debug_logging_);

  listener::SocketBinding binding{server_fd_.load(), port_};
  if (binding.fd < 0)
  {
    try
    {
      binding = listener_socket_.open(host_, port_);
    }
    catch (...)
    {
      lifecycle_.mark_failed(lifecycle::StopReason::startup_failure);
      throw;
    }
  }

  port_ = binding.port;
  server_fd_.store(binding.fd);
  if (!lifecycle_.mark_listening(binding.fd, binding.port) || server_fd_.load() < 0)
  {
    close_listener_fd(false);
    lifecycle_.finish_stop();
    return;
  }
  lifecycle_.mark_boot_ready();
  log_listening_banner(host_, port_);

  try
  {
    for (;;)
    {
      const lifecycle::Snapshot snapshot = lifecycle_.snapshot();
      if (snapshot.state == lifecycle::State::draining || snapshot.state == lifecycle::State::failed)
      {
        break;
      }

      if (VajraNative::shutdown_requested())
      {
        if (shutdown_begin_callback_)
        {
          shutdown_begin_callback_();
        }
        lifecycle_.request_stop(lifecycle::StopReason::signal_shutdown);
        break;
      }

      const int listener_fd = server_fd_.load();
      if (listener_fd < 0)
      {
        break;
      }

      pollfd listener_descriptor{listener_fd, POLLIN, 0};
      const int poll_result = poll(&listener_descriptor, 1, kHandlerReapPollTimeoutMilliseconds);
      if (poll_result == 0)
      {
        reap_completed_handler_threads();
        continue;
      }
      if (poll_result < 0)
      {
        if (errno == EINTR)
        {
          continue;
        }

        log_accept_failed(std::strerror(errno));
        continue;
      }

      sockaddr_in client_addr{};
      socklen_t client_len = sizeof(client_addr);

      const int client_fd = accept(listener_fd, reinterpret_cast<sockaddr *>(&client_addr), &client_len);
      if (client_fd < 0)
      {
        if (lifecycle_.snapshot().state == lifecycle::State::draining || VajraNative::shutdown_requested())
        {
          if (VajraNative::shutdown_requested())
          {
            if (shutdown_begin_callback_)
            {
              shutdown_begin_callback_();
            }
            lifecycle_.request_stop(lifecycle::StopReason::signal_shutdown);
          }
          break;
        }

        if (errno == EINTR)
        {
          continue;
        }

        log_accept_failed(std::strerror(errno));
        continue;
      }

      if (lifecycle_.snapshot().state == lifecycle::State::listening)
      {
        lifecycle_.mark_serving();
      }

      reap_completed_handler_threads();

      const std::size_t previous_active_connections = active_connection_count_.fetch_add(1, std::memory_order_acq_rel);
      if (previous_active_connections >= max_connections_)
      {
        active_connection_count_.fetch_sub(1, std::memory_order_acq_rel);
        log_connection_rejected(max_connections_);
        close(client_fd);
        continue;
      }

      const std::shared_ptr<std::atomic<bool>> completed = std::make_shared<std::atomic<bool>>(false);
      try
      {
        std::lock_guard<std::mutex> lock(handler_threads_mutex_);
        handler_threads_.push_back(HandlerThread{std::thread(), completed});
        HandlerThread &handler_thread = handler_threads_.back();
        handler_thread.thread = std::thread([this, client_fd, client_addr, completed]() {
          ActiveConnectionGuard active_connection_guard(active_connection_count_);
          try
          {
            request_processor_.handle(client_fd, socket_context_for(client_fd, client_addr, port_));
          }
          catch (const std::exception &error)
          {
            log_handler_thread_failure(client_addr, client_fd, error.what());
            if (fcntl(client_fd, F_GETFD) != -1 || errno != EBADF)
            {
              close(client_fd);
            }
          }
          catch (...)
          {
            log_handler_thread_failure(client_addr, client_fd, "unknown exception");
            if (fcntl(client_fd, F_GETFD) != -1 || errno != EBADF)
            {
              close(client_fd);
            }
          }
          completed->store(true, std::memory_order_release);
        });
      }
      catch (...)
      {
        std::lock_guard<std::mutex> lock(handler_threads_mutex_);
        if (!handler_threads_.empty() && handler_threads_.back().thread.joinable() == false &&
            handler_threads_.back().completed == completed)
        {
          handler_threads_.pop_back();
        }
        active_connection_count_.fetch_sub(1, std::memory_order_acq_rel);
        close(client_fd);
        throw;
      }
    }
  }
  catch (...)
  {
    close_listener_fd(false);
    join_handler_threads();
    lifecycle_.finish_stop();
    throw;
  }

  close_listener_fd(false);
  join_handler_threads();
  lifecycle_.finish_stop();
}

void Vajra::Server::stop()
{
  if (shutdown_begin_callback_)
  {
    shutdown_begin_callback_();
  }
  lifecycle_.request_stop(lifecycle::StopReason::programmatic_stop);
  close_listener_fd(true);
}

Vajra::lifecycle::Snapshot Vajra::Server::lifecycle_snapshot() const
{
  return lifecycle_.snapshot();
}

void Vajra::Server::set_lifecycle_observer(lifecycle::Controller::Observer observer)
{
  lifecycle_.set_observer(std::move(observer));
}
