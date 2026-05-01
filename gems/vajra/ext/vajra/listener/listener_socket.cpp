// Copyright Codevedas Inc. 2025-present
//
// This source code is licensed under the MIT license found in the
// LICENSE file in the root directory of this source tree.

#include "listener_socket.hpp"

#include <arpa/inet.h>
#include <cerrno>
#include <cstring>
#include <stdexcept>
#include <string>
#include <sys/socket.h>
#include <unistd.h>

namespace
{
  std::runtime_error startup_error(const char *stage, int port, int error_number)
  {
    return std::runtime_error(
        std::string("listener ") + stage + " failed for port " + std::to_string(port) + ": " +
        std::strerror(error_number));
  }
}

Vajra::listener::SocketBinding Vajra::listener::Socket::open(int port) const
{
  const int socket_fd = socket(AF_INET, SOCK_STREAM, 0);
  if (socket_fd < 0)
  {
    throw startup_error("socket creation", port, errno);
  }

  int opt = 1;
  if (setsockopt(socket_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt)) < 0)
  {
    const int error_number = errno;
    close(socket_fd);
    throw startup_error("socket option setup", port, error_number);
  }

  sockaddr_in addr{};
  addr.sin_family = AF_INET;
  addr.sin_addr.s_addr = INADDR_ANY;
  addr.sin_port = htons(port);

  if (bind(socket_fd, reinterpret_cast<sockaddr *>(&addr), sizeof(addr)) < 0)
  {
    const int error_number = errno;
    close(socket_fd);
    throw startup_error("bind", port, error_number);
  }

  sockaddr_in bound_addr{};
  socklen_t bound_addr_len = sizeof(bound_addr);
  if (getsockname(socket_fd, reinterpret_cast<sockaddr *>(&bound_addr), &bound_addr_len) < 0)
  {
    const int error_number = errno;
    close(socket_fd);
    throw startup_error("bound port discovery", port, error_number);
  }

  const int bound_port = ntohs(bound_addr.sin_port);

  if (listen(socket_fd, 128) < 0)
  {
    const int error_number = errno;
    close(socket_fd);
    throw startup_error("listen", bound_port, error_number);
  }

  return SocketBinding{socket_fd, bound_port};
}
