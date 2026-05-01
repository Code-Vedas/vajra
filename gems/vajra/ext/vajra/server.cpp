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
#include <sys/socket.h>
#include <unistd.h>

Vajra::Server::Server(int port, std::size_t max_request_head_bytes)
    : port_(port),
      server_fd_(-1),
      running_(false),
      stop_requested_(false),
      listener_socket_(),
      request_processor_(max_request_head_bytes)
{
}

Vajra::Server::~Server()
{
  close_listener_fd(false);
}

void Vajra::Server::close_listener_fd(bool interrupt_accept)
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

void Vajra::Server::start()
{
  if (stop_requested_.load())
  {
    return;
  }

  const listener::SocketBinding binding = listener_socket_.open(port_);
  port_ = binding.port;
  server_fd_.store(binding.fd);

  if (stop_requested_.load())
  {
    close_listener_fd(false);
    return;
  }

  running_ = true;

  if (stop_requested_.load() || server_fd_.load() < 0)
  {
    close_listener_fd(false);
    return;
  }

  std::cout << "Vajra listening on port " << port_ << std::endl;

  while (running_)
  {
    if (stop_requested_.load() || VajraNative::shutdown_requested())
    {
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
      if (!running_ || stop_requested_.load() || VajraNative::shutdown_requested())
      {
        break;
      }

      if (errno == EINTR)
      {
        continue;
      }

      std::cerr << "accept failed: " << std::strerror(errno) << std::endl;
      continue;
    }

    request_processor_.handle(client_fd);
  }

  close_listener_fd(false);
}

void Vajra::Server::stop()
{
  stop_requested_ = true;
  close_listener_fd(true);
}
