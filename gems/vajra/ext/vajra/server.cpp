// Copyright Codevedas Inc. 2025-present
//
// This source code is licensed under the MIT license found in the
// LICENSE file in the root directory of this source tree.

#include "server.hpp"
#include "vajra.hpp"
#include "response/response_writer.hpp"
#include "runtime/time_utils.hpp"
#include "runtime/runtime_state.hpp"

#include <algorithm>
#include <arpa/inet.h>
#include <cerrno>
#include <cstring>
#include <cstdint>
#include <fcntl.h>
#include <iostream>
#include <limits>
#include <poll.h>
#include <sstream>
#include <stdexcept>
#include <sys/resource.h>
#include <sys/socket.h>
#include <unistd.h>
#include <utility>

namespace
{
  constexpr const char *kUnknownSocketAddress = "0.0.0.0";
  constexpr int kHandlerReapPollTimeoutMilliseconds = 1000;
  constexpr std::size_t kRuntimeFdReserve = 32;
  constexpr std::size_t kListenerFdCost = 1;
  constexpr std::size_t kAcceptedClientSocketFdCost = 1;

  void log_client_socket_interrupt_failed(int client_fd, const char *error_message);

  std::size_t checked_add(std::size_t left, std::size_t right, const char *error_message)
  {
    if (right > (std::numeric_limits<std::size_t>::max() - left))
    {
      throw std::runtime_error(error_message);
    }

    return left + right;
  }

  std::size_t active_client_descriptor_reservation()
  {
    return checked_add(
        kAcceptedClientSocketFdCost,
        0,
        "invalid active client descriptor reservation");
  }

  std::size_t safe_tracked_client_descriptor_limit()
  {
    rlimit fd_limit{};
    if (getrlimit(RLIMIT_NOFILE, &fd_limit) != 0 || fd_limit.rlim_cur == RLIM_INFINITY)
    {
      return std::numeric_limits<std::size_t>::max();
    }

    const std::size_t available_fds = static_cast<std::size_t>(fd_limit.rlim_cur);
    const std::size_t fixed_fd_cost = checked_add(
        kRuntimeFdReserve,
        kListenerFdCost,
        "invalid max_connections: fixed fd cost is too large");
    if (available_fds <= fixed_fd_cost)
    {
      return 0;
    }

    return available_fds - fixed_fd_cost;
  }

#if !defined(__linux__) || !defined(SOCK_CLOEXEC)
  int set_fd_cloexec(int fd)
  {
    const int existing_flags = fcntl(fd, F_GETFD);
    if (existing_flags < 0)
    {
      return -1;
    }

    return fcntl(fd, F_SETFD, existing_flags | FD_CLOEXEC);
  }
#endif

  int accept_client_cloexec(int listener_fd, sockaddr *client_addr, socklen_t *client_len)
  {
#if defined(__linux__) && defined(SOCK_CLOEXEC)
    return accept4(listener_fd, client_addr, client_len, SOCK_CLOEXEC);
#else
    const int client_fd = accept(listener_fd, client_addr, client_len);
    if (client_fd < 0)
    {
      return client_fd;
    }

    if (set_fd_cloexec(client_fd) != 0)
    {
      const int error_number = errno;
      close(client_fd);
      errno = error_number;
      return -1;
    }

    return client_fd;
#endif
  }

  bool shutdown_interrupt_succeeded_or_expected(int fd, int original_fd)
  {
    for (;;)
    {
      if (shutdown(fd, SHUT_RDWR) == 0)
      {
        return true;
      }

      if (errno == EINTR)
      {
        continue;
      }
      if (errno == ENOTCONN || errno == EINVAL || errno == ENOTSOCK)
      {
        return true;
      }

      log_client_socket_interrupt_failed(original_fd, std::strerror(errno));
      return false;
    }
  }

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

  void log_message(const char *event_type, const std::string &message, std::ostream &stream)
  {
    stream << "[Vajra][" << event_type << "] " << Vajra::runtime::utc_timestamp() << ' ' << message << std::endl;
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

  void log_active_client_tracking_failed(const char *error_message)
  {
    std::ostringstream message;
    message << "active client tracking failed: " << error_message;
    log_message("error", message.str(), std::cerr);
  }

  void log_poll_failed(const char *error_message)
  {
    std::ostringstream message;
    message << "poll failed: " << error_message;
    log_message("error", message.str(), std::cerr);
  }

  void log_poll_listener_event(short revents)
  {
    std::ostringstream message;
    message << "poll reported listener error: revents=" << revents;
    log_message("error", message.str(), std::cerr);
  }

  void log_connection_rejected(std::size_t max_connections)
  {
    std::ostringstream message;
    message << "connection rejected: active connection limit reached (max_connections=" << max_connections << ")";
    log_message("error", message.str(), std::cerr);
  }

  void log_handler_thread_failure(const Vajra::request::SocketContext &socket_context, int client_fd, const std::string &message)
  {
    std::ostringstream error_message;
    error_message << "handler thread failed for client="
                  << socket_context.remote_address << ':' << socket_context.remote_port
                  << " client_fd=" << client_fd
                  << " error=" << message;
    log_message("error", error_message.str(), std::cerr);
  }

  void log_client_socket_interrupt_failed(int client_fd, const char *error_message)
  {
    std::ostringstream message;
    message << "client socket interrupt failed: client_fd=" << client_fd << " error=" << error_message;
    log_message("error", message.str(), std::cerr);
  }

  void log_client_socket_interrupt_aborted(const char *error_message)
  {
    std::ostringstream message;
    message << "client socket interrupt aborted: " << error_message;
    log_message("error", message.str(), std::cerr);
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
    int request_body_timeout_seconds,
    int persistent_timeout_seconds,
    std::size_t max_connections,
    std::function<void()> shutdown_begin_callback,
    std::size_t max_request_body_bytes)
    : host_(std::move(host)),
      port_(port),
      server_fd_(inherited_listener_fd),
      listener_socket_(),
      request_processor_(
          max_request_head_bytes,
          max_request_body_bytes,
          request_head_timeout_seconds,
          first_data_timeout_seconds,
          request_body_timeout_seconds,
          persistent_timeout_seconds,
          0,
          std::move(request_executor)),
      lifecycle_(),
      process_role_(std::move(process_role)),
      runtime_mode_(std::move(runtime_mode)),
      worker_processes_(worker_processes),
      request_execution_role_(std::move(request_execution_role)),
      debug_logging_(debug_logging),
      max_connections_(max_connections),
      max_tracked_client_descriptors_(safe_tracked_client_descriptor_limit()),
      shutdown_begin_callback_(std::move(shutdown_begin_callback))
{
  if (max_tracked_client_descriptors_ < active_client_descriptor_reservation())
  {
    throw std::runtime_error(
        "invalid max_connections: fd limit is too low to keep even one accepted connection open");
  }

  set_lifecycle_observer([this](lifecycle::HookPoint hook_point, const lifecycle::Snapshot &snapshot)
                         {
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

    throw std::logic_error("unknown lifecycle hook point"); });
}

Vajra::Server::~Server()
{
  close_listener_fd(false);
  const lifecycle::Snapshot snapshot = lifecycle_.snapshot();
  if (snapshot.state == lifecycle::State::draining || snapshot.state == lifecycle::State::failed)
  {
    interrupt_active_client_sockets();
  }
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
    {
      std::lock_guard<std::mutex> queue_lock(connection_queue_mutex_);
      connection_workers_stopping_ = true;
    }
    connection_queue_condition_.notify_all();

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

void Vajra::Server::start_handler_threads()
{
  std::lock_guard<std::mutex> lock(handler_threads_mutex_);
  if (!handler_threads_.empty())
  {
    return;
  }

  {
    std::lock_guard<std::mutex> queue_lock(connection_queue_mutex_);
    connection_workers_stopping_ = false;
    pending_clients_.clear();
  }

  const std::size_t handler_count = std::max<std::size_t>(1, max_connections_);
  handler_threads_.reserve(handler_count);
  try
  {
    for (std::size_t i = 0; i < handler_count; ++i)
    {
      handler_threads_.push_back(HandlerThread{std::thread([this]()
                                                           { run_handler_thread(); })});
    }
  }
  catch (...)
  {
    {
      std::lock_guard<std::mutex> queue_lock(connection_queue_mutex_);
      connection_workers_stopping_ = true;
    }
    connection_queue_condition_.notify_all();
    throw;
  }
}

std::uint64_t Vajra::Server::register_active_client_fd(int client_fd)
{
  const std::size_t descriptor_count = active_client_descriptor_reservation();
  const std::uint64_t client_token = next_active_client_token_.fetch_add(1, std::memory_order_acq_rel) + 1;
  {
    std::lock_guard<std::mutex> lock(active_client_fds_mutex_);
    if (max_tracked_client_descriptors_ != std::numeric_limits<std::size_t>::max() &&
        (active_tracked_client_descriptors_ > max_tracked_client_descriptors_ ||
         descriptor_count > max_tracked_client_descriptors_ - active_tracked_client_descriptors_))
    {
      throw std::runtime_error("active client descriptor budget exhausted");
    }

    active_client_fds_.emplace(client_token, ActiveClientRegistration{client_fd, true, descriptor_count});
    active_tracked_client_descriptors_ += descriptor_count;
  }

  return client_token;
}

void Vajra::Server::unregister_active_client_fd(int client_fd, std::uint64_t client_token)
{
  std::size_t descriptor_count = 0;
  {
    std::lock_guard<std::mutex> lock(active_client_fds_mutex_);
    const auto active_client_fd = active_client_fds_.find(client_token);
    if (active_client_fd == active_client_fds_.end() || active_client_fd->second.original_fd != client_fd)
    {
      return;
    }

    descriptor_count = active_client_fd->second.descriptor_count;
    active_client_fd->second.open = false;
    active_client_fds_.erase(active_client_fd);
    if (active_tracked_client_descriptors_ >= descriptor_count)
    {
      active_tracked_client_descriptors_ -= descriptor_count;
    }
    else
    {
      active_tracked_client_descriptors_ = 0;
    }
  }
}

void Vajra::Server::interrupt_active_client_sockets() noexcept
{
  try
  {
    std::lock_guard<std::mutex> lock(active_client_fds_mutex_);
    for (auto &[client_token, registration] : active_client_fds_)
    {
      (void)client_token;
      if (!registration.open)
      {
        continue;
      }

      shutdown_interrupt_succeeded_or_expected(registration.original_fd, registration.original_fd);
      registration.open = false;
    }
  }
  catch (const std::exception &error)
  {
    log_client_socket_interrupt_aborted(error.what());
  }
  catch (...)
  {
    log_client_socket_interrupt_aborted("unknown native error");
  }
}

void Vajra::Server::reap_completed_handler_threads()
{
}

void Vajra::Server::enqueue_pending_client(PendingClient client)
{
  {
    std::lock_guard<std::mutex> lock(connection_queue_mutex_);
    if (connection_workers_stopping_)
    {
      throw std::runtime_error("connection worker pool is stopping");
    }

    pending_clients_.push_back(std::move(client));
  }
  connection_queue_condition_.notify_one();
}

void Vajra::Server::run_handler_thread()
{
  for (;;)
  {
    PendingClient client{};
    {
      std::unique_lock<std::mutex> lock(connection_queue_mutex_);
      connection_queue_condition_.wait(lock, [this]()
                                       { return connection_workers_stopping_ || !pending_clients_.empty(); });
      if (pending_clients_.empty())
      {
        if (connection_workers_stopping_)
        {
          return;
        }
        continue;
      }

      client = std::move(pending_clients_.front());
      pending_clients_.pop_front();
    }

    handle_pending_client(std::move(client));
  }
}

void Vajra::Server::handle_pending_client(PendingClient client)
{
  ActiveConnectionGuard active_connection_guard(active_connection_count_);
  Vajra::runtime::note_worker_connection_opened();
  bool hijacked = false;
  try
  {
    Vajra::transport::PlainConnection connection(client.fd);
    const Vajra::request::RequestProcessingOutcome outcome = request_processor_.handle(connection, client.socket_context);
    hijacked = outcome == Vajra::request::RequestProcessingOutcome::hijacked;
  }
  catch (const std::exception &error)
  {
    log_handler_thread_failure(client.socket_context, client.fd, error.what());
  }
  catch (...)
  {
    log_handler_thread_failure(client.socket_context, client.fd, "unknown exception");
  }
  unregister_active_client_fd(client.fd, client.token);
  if (!hijacked && client.fd >= 0)
  {
    close(client.fd);
  }
  Vajra::runtime::note_worker_connection_closed();
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
    start_handler_threads();

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
        interrupt_active_client_sockets();
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

        log_poll_failed(std::strerror(errno));
        continue;
      }
      if ((listener_descriptor.revents & (POLLERR | POLLHUP | POLLNVAL)) != 0)
      {
        const lifecycle::Snapshot error_snapshot = lifecycle_.snapshot();
        if (error_snapshot.state == lifecycle::State::draining ||
            error_snapshot.state == lifecycle::State::failed ||
            server_fd_.load() < 0 ||
            VajraNative::shutdown_requested())
        {
          break;
        }

        log_poll_listener_event(listener_descriptor.revents);
        lifecycle_.mark_failed(lifecycle::StopReason::listener_failure);
        break;
      }
      if ((listener_descriptor.revents & POLLIN) == 0)
      {
        reap_completed_handler_threads();
        continue;
      }

      sockaddr_in client_addr{};
      socklen_t client_len = sizeof(client_addr);

      const int client_fd = accept_client_cloexec(listener_fd, reinterpret_cast<sockaddr *>(&client_addr), &client_len);
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
            interrupt_active_client_sockets();
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
      Vajra::runtime::note_worker_accept();

      reap_completed_handler_threads();
      const std::size_t previous_active_connections = active_connection_count_.fetch_add(1, std::memory_order_acq_rel);
      if (previous_active_connections >= max_connections_)
      {
        active_connection_count_.fetch_sub(1, std::memory_order_acq_rel);
        log_connection_rejected(max_connections_);
        close(client_fd);
        continue;
      }
      Vajra::response::ResponseWriter::prepare_client_socket(client_fd);

      std::uint64_t client_token = 0;
      try
      {
        client_token = register_active_client_fd(client_fd);
      }
      catch (const std::exception &error)
      {
        active_connection_count_.fetch_sub(1, std::memory_order_acq_rel);
        log_active_client_tracking_failed(error.what());
        close(client_fd);
        continue;
      }

      try
      {
        enqueue_pending_client(PendingClient{
            client_fd,
            socket_context_for(client_fd, client_addr, port_),
            client_token});
      }
      catch (...)
      {
        active_connection_count_.fetch_sub(1, std::memory_order_acq_rel);
        unregister_active_client_fd(client_fd, client_token);
        close(client_fd);
        throw;
      }
    }
  }
  catch (...)
  {
    close_listener_fd(false);
    const lifecycle::Snapshot snapshot = lifecycle_.snapshot();
    if (snapshot.state == lifecycle::State::draining || snapshot.state == lifecycle::State::failed)
    {
      interrupt_active_client_sockets();
    }
    join_handler_threads();
    lifecycle_.finish_stop();
    throw;
  }

  close_listener_fd(false);
  const lifecycle::Snapshot snapshot = lifecycle_.snapshot();
  if (snapshot.state == lifecycle::State::draining || snapshot.state == lifecycle::State::failed)
  {
    interrupt_active_client_sockets();
  }
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
  interrupt_active_client_sockets();
}

Vajra::lifecycle::Snapshot Vajra::Server::lifecycle_snapshot() const
{
  return lifecycle_.snapshot();
}

void Vajra::Server::set_lifecycle_observer(lifecycle::Controller::Observer observer)
{
  lifecycle_.set_observer(std::move(observer));
}
