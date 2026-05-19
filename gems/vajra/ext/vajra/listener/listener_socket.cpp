// Copyright Codevedas Inc. 2025-present
//
// This source code is licensed under the MIT license found in the
// LICENSE file in the root directory of this source tree.

#include "listener_socket.hpp"

#include <arpa/inet.h>
#include <cerrno>
#include <cstring>
#include <netdb.h>
#include <stdexcept>
#include <string>
#include <sys/socket.h>
#include <unistd.h>

namespace
{
  std::runtime_error startup_error(const char *stage, const std::string &host, int port, int error_number)
  {
    return std::runtime_error(
        std::string("listener ") + stage + " failed for " + host + ":" + std::to_string(port) + ": " +
        std::strerror(error_number));
  }

  std::runtime_error host_resolution_error(const std::string &host, int port, int status)
  {
    return std::runtime_error(
        "listener host resolution failed for " + host + ":" + std::to_string(port) + ": " +
        gai_strerror(status));
  }
}

Vajra::listener::SocketBinding Vajra::listener::Socket::open(const std::string &host, int port) const
{
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

  int socket_fd = -1;
  for (addrinfo *candidate = result; candidate != nullptr; candidate = candidate->ai_next)
  {
    socket_fd = socket(candidate->ai_family, candidate->ai_socktype, candidate->ai_protocol);
    if (socket_fd < 0)
    {
      continue;
    }

    int opt = 1;
    if (setsockopt(socket_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt)) < 0)
    {
      const int error_number = errno;
      close(socket_fd);
      freeaddrinfo(result);
      throw startup_error("socket option setup", host, port, error_number);
    }

    if (bind(socket_fd, candidate->ai_addr, candidate->ai_addrlen) == 0)
    {
      break;
    }

    close(socket_fd);
    socket_fd = -1;
  }

  freeaddrinfo(result);

  if (socket_fd < 0)
  {
    throw startup_error("bind", host, port, errno);
  }

  sockaddr_in bound_addr{};
  socklen_t bound_addr_len = sizeof(bound_addr);
  if (getsockname(socket_fd, reinterpret_cast<sockaddr *>(&bound_addr), &bound_addr_len) < 0)
  {
    const int error_number = errno;
    close(socket_fd);
    throw startup_error("bound port discovery", host, port, error_number);
  }

  const int bound_port = ntohs(bound_addr.sin_port);

  if (listen(socket_fd, 128) < 0)
  {
    const int error_number = errno;
    close(socket_fd);
    throw startup_error("listen", host, bound_port, error_number);
  }

  return SocketBinding{socket_fd, bound_port};
}
