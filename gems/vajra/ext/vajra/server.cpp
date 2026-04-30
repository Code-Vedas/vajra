// Copyright Codevedas Inc. 2025-present
//
// This source code is licensed under the MIT license found in the
// LICENSE file in the root directory of this source tree.

#include "server.hpp"
#include "vajra.hpp"

#include <arpa/inet.h>
#include <cerrno>
#include <cstring>
#include <iostream>
#include <stdexcept>
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

Server::Server(int port) : port_(port), server_fd_(-1), running_(false), stop_requested_(false) {}

Server::~Server()
{
  close_listener_fd(false);
}

void Server::setup_socket()
{
  const int socket_fd = socket(AF_INET, SOCK_STREAM, 0);
  if (socket_fd < 0)
  {
    throw startup_error("socket creation", port_, errno);
  }

  int opt = 1;
  if (setsockopt(socket_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt)) < 0)
  {
    const int error_number = errno;
    close(socket_fd);
    throw startup_error("socket option setup", port_, error_number);
  }

  sockaddr_in addr{};
  addr.sin_family = AF_INET;
  addr.sin_addr.s_addr = INADDR_ANY;
  addr.sin_port = htons(port_);

  if (bind(socket_fd, reinterpret_cast<sockaddr *>(&addr), sizeof(addr)) < 0)
  {
    const int error_number = errno;
    close(socket_fd);
    throw startup_error("bind", port_, error_number);
  }

  sockaddr_in bound_addr{};
  socklen_t bound_addr_len = sizeof(bound_addr);
  if (getsockname(socket_fd, reinterpret_cast<sockaddr *>(&bound_addr), &bound_addr_len) < 0)
  {
    const int error_number = errno;
    close(socket_fd);
    throw startup_error("bound port discovery", port_, error_number);
  }

  port_ = ntohs(bound_addr.sin_port);

  if (listen(socket_fd, 128) < 0)
  {
    const int error_number = errno;
    close(socket_fd);
    throw startup_error("listen", port_, error_number);
  }

  server_fd_.store(socket_fd);
}

void Server::close_listener_fd(bool interrupt_accept)
{
  running_ = false;

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

void Server::start()
{
  setup_socket();

  if (stop_requested_.load())
  {
    close_listener_fd(false);
    return;
  }

  running_ = true;

  std::cout << "Vajra listening on port " << port_ << std::endl;

  while (running_)
  {
    const int listener_fd = server_fd_.load();
    if (listener_fd < 0)
    {
      break;
    }

    sockaddr_in client_addr{};
    socklen_t client_len = sizeof(client_addr);

    int client_fd = accept(listener_fd, reinterpret_cast<sockaddr *>(&client_addr), &client_len);
    if (client_fd < 0)
    {
      if (!running_ || stop_requested_.load() || VajraNative::shutdown_requested())
      {
        break;
      }
      std::cerr << "accept failed: " << std::strerror(errno) << std::endl;
      continue;
    }

    char buffer[4096];
    while (true)
    {
      ssize_t bytes_read = recv(client_fd, buffer, sizeof(buffer), 0);
      if (bytes_read < 0)
      {
        std::cerr << "recv failed: " << std::strerror(errno) << std::endl;
        break;
      }

      if (bytes_read == 0)
      {
        break;
      }

      std::string chunk(buffer, bytes_read);
      if (chunk.find("\r\n\r\n") != std::string::npos)
      {
        break;
      }
    }

    const char *response =
        "HTTP/1.1 200 OK\r\n"
        "Content-Length: 2\r\n"
        "Content-Type: text/plain\r\n"
        "Connection: close\r\n"
        "\r\n"
        "OK";

    send(client_fd, response, std::strlen(response), 0);
    close(client_fd);
  }

  close_listener_fd(false);
}

void Server::stop()
{
  stop_requested_ = true;
  close_listener_fd(true);
}
