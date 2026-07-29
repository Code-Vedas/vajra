// Copyright Codevedas Inc. 2025-present
//
// This source code is licensed under the MIT license found in the
// LICENSE file in the root directory of this source tree.

#include "listener_socket.hpp"

#include <cerrno>
#include <cstring>
#include <memory>
#include <stdexcept>
#include <string>

#ifndef _WIN32
#include <arpa/inet.h>
#include <netdb.h>
#endif

namespace
{
  enum class SocketFailureStage
  {
    socket_create,
    bind
  };

  std::runtime_error startup_error(const char *stage, const std::string &host, int port, int error_number)
  {
    return std::runtime_error(
        std::string("listener ") + stage + " failed for " + host + ":" + std::to_string(port) + ": " +
        Vajra::platform::socket_error_message(error_number));
  }

  std::runtime_error host_resolution_error(const std::string &host, int port, int status)
  {
    return std::runtime_error(
        "listener host resolution failed for " + host + ":" + std::to_string(port) + ": " +
        gai_strerror(status));
  }
}

Vajra::listener::SocketBinding Vajra::listener::Socket::open(const std::string &host, int port, bool reuse_port) const
{
  platform::ensure_socket_runtime();
  addrinfo hints{};
  hints.ai_family = AF_INET;
  hints.ai_socktype = SOCK_STREAM;
  hints.ai_flags = host == "0.0.0.0" ? AI_PASSIVE : 0;

  addrinfo *result = nullptr;
  const std::string port_string = std::to_string(port);
  const int resolution_status = getaddrinfo(
      host == "0.0.0.0" ? nullptr : host.c_str(),
      port_string.c_str(),
      &hints,
      &result);
  if (resolution_status != 0)
  {
    throw host_resolution_error(host, port, resolution_status);
  }
  const std::unique_ptr<addrinfo, decltype(&freeaddrinfo)> addresses(result, freeaddrinfo);

  platform::SocketHandle socket_fd = platform::kInvalidSocket;
  int last_error = 0;
  SocketFailureStage last_failure_stage = SocketFailureStage::bind;
  for (addrinfo *candidate = addresses.get(); candidate != nullptr; candidate = candidate->ai_next)
  {
    socket_fd = platform::create_tcp_socket(candidate->ai_family, candidate->ai_socktype, candidate->ai_protocol);
    if (!platform::socket_valid(socket_fd))
    {
      last_error = platform::socket_last_error();
      last_failure_stage = SocketFailureStage::socket_create;
      continue;
    }

    int opt = 1;
    if (!platform::set_socket_option(socket_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt)))
    {
      const int error_number = platform::socket_last_error();
      platform::close_socket(socket_fd);
      throw std::runtime_error(
          startup_error("socket option setup", host, port, error_number).what() +
          std::string(" (native_handle=") + std::to_string(platform::socket_handle_value(socket_fd)) + ")");
    }

#ifdef SO_REUSEPORT
    if (reuse_port && !platform::set_socket_option(socket_fd, SOL_SOCKET, SO_REUSEPORT, &opt, sizeof(opt)))
    {
      const int error_number = platform::socket_last_error();
      platform::close_socket(socket_fd);
      throw startup_error("reuseport setup", host, port, error_number);
    }
#else
    if (reuse_port)
    {
      platform::close_socket(socket_fd);
      throw std::runtime_error("listener reuse_port requested but SO_REUSEPORT is not available");
    }
#endif

    if (platform::bind_socket(socket_fd, candidate->ai_addr, static_cast<socklen_t>(candidate->ai_addrlen)))
    {
      break;
    }

    last_error = platform::socket_last_error();
    last_failure_stage = SocketFailureStage::bind;
    platform::close_socket(socket_fd);
    socket_fd = platform::kInvalidSocket;
  }

  if (!platform::socket_valid(socket_fd))
  {
    throw startup_error(last_failure_stage == SocketFailureStage::socket_create ? "socket create" : "bind", host, port, last_error);
  }

  sockaddr_in bound_addr{};
  socklen_t bound_addr_len = sizeof(bound_addr);
  if (!platform::socket_name(socket_fd, reinterpret_cast<sockaddr *>(&bound_addr), &bound_addr_len))
  {
    const int error_number = platform::socket_last_error();
    platform::close_socket(socket_fd);
    throw startup_error("bound port discovery", host, port, error_number);
  }

  const int bound_port = ntohs(bound_addr.sin_port);

  if (!platform::listen_socket(socket_fd, 128))
  {
    const int error_number = platform::socket_last_error();
    platform::close_socket(socket_fd);
    throw startup_error("listen", host, bound_port, error_number);
  }

#ifdef _WIN32
  if (!platform::set_socket_nonblocking(socket_fd, true))
  {
    const int error_number = platform::socket_last_error();
    platform::close_socket(socket_fd);
    throw startup_error("nonblocking setup", host, bound_port, error_number);
  }
#endif

  return SocketBinding{socket_fd, bound_port};
}
