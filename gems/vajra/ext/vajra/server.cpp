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
#include <iomanip>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <sys/socket.h>
#include <unistd.h>
#include <utility>

namespace
{
  constexpr const char *kUnknownSocketAddress = "0.0.0.0";

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

  std::string lifecycle_details(const Vajra::lifecycle::Snapshot &snapshot, const std::string &runtime_role)
  {
    std::ostringstream message;
    message << "state=" << state_name(snapshot.state)
            << " boot_status=" << boot_readiness_name(snapshot.boot_readiness)
            << " stop_reason=" << stop_reason_name(snapshot.last_stop_reason)
            << " port=" << snapshot.port
            << " listener_owned=" << (snapshot.listener_owned ? "true" : "false")
            << " listener_fd=" << snapshot.listener_fd
            << " mode=single_process"
            << " runtime_role=" << runtime_role
            << " worker_processes=0";
    return message.str();
  }

  void log_runtime_event(
      const char *event_name,
      const Vajra::lifecycle::Snapshot &snapshot,
      const std::string &runtime_role,
      std::ostream &stream)
  {
    std::ostringstream message;
    message << "event=" << event_name << ' ' << lifecycle_details(snapshot, runtime_role);
    log_message("lifecycle", message.str(), stream);
  }

  void log_booting_event(const std::string &runtime_role)
  {
    const Vajra::lifecycle::Snapshot snapshot{
        Vajra::lifecycle::State::booting,
        Vajra::lifecycle::BootReadiness::pending,
        Vajra::lifecycle::StopReason::none,
        false,
        -1,
        -1,
    };
    log_runtime_event("booting", snapshot, runtime_role, std::cout);
  }

  void log_listening_banner(int port)
  {
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
}

Vajra::Server::Server(
    int port,
    std::size_t max_request_head_bytes,
    std::shared_ptr<const request::RequestExecutor> request_executor,
    std::string runtime_role)
    : port_(port),
      server_fd_(-1),
      listener_socket_(),
      request_processor_(max_request_head_bytes, std::move(request_executor)),
      lifecycle_(),
      runtime_role_(std::move(runtime_role))
{
  set_lifecycle_observer([this](lifecycle::HookPoint hook_point, const lifecycle::Snapshot &snapshot) {
    switch (hook_point)
    {
      case lifecycle::HookPoint::boot_complete:
        log_runtime_event("boot_complete", snapshot, runtime_role_, std::cout);
        return;
      case lifecycle::HookPoint::serving_entered:
        log_runtime_event("serving_entered", snapshot, runtime_role_, std::cout);
        return;
      case lifecycle::HookPoint::drain_requested:
        log_runtime_event("drain_requested", snapshot, runtime_role_, std::cout);
        return;
      case lifecycle::HookPoint::stop_completed:
        log_runtime_event("stop_completed", snapshot, runtime_role_, std::cout);
        return;
      case lifecycle::HookPoint::failed_entered:
        log_runtime_event("failed_entered", snapshot, runtime_role_, std::cerr);
        return;
    }

    throw std::logic_error("unknown lifecycle hook point");
  });
}

Vajra::Server::~Server()
{
  close_listener_fd(false);
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

void Vajra::Server::start()
{
  if (!lifecycle_.begin_startup())
  {
    close_listener_fd(false);
    return;
  }

  log_booting_event(runtime_role_);

  listener::SocketBinding binding;
  try
  {
    binding = listener_socket_.open(port_);
  }
  catch (...)
  {
    lifecycle_.mark_failed(lifecycle::StopReason::startup_failure);
    throw;
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
  log_listening_banner(port_);

  for (;;)
  {
    const lifecycle::Snapshot snapshot = lifecycle_.snapshot();
    if (snapshot.state == lifecycle::State::draining || snapshot.state == lifecycle::State::failed)
    {
      break;
    }

    if (VajraNative::shutdown_requested())
    {
      lifecycle_.request_stop(lifecycle::StopReason::signal_shutdown);
      break;
    }

    const int listener_fd = server_fd_.load();
    if (listener_fd < 0)
    {
      break;
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

    request_processor_.handle(client_fd, socket_context_for(client_fd, client_addr, port_));
  }

  close_listener_fd(false);
  lifecycle_.finish_stop();
}

void Vajra::Server::stop()
{
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
