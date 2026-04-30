// Copyright Codevedas Inc. 2025-present
//
// This source code is licensed under the MIT license found in the
// LICENSE file in the root directory of this source tree.

#include "server.hpp"

#include <arpa/inet.h>
#include <cerrno>
#include <cstring>
#include <iostream>
#include <stdexcept>
#include <sys/socket.h>
#include <unistd.h>

Server::Server(int port) : port_(port), server_fd_(-1), running_(false) {}

Server::~Server()
{
  if (server_fd_ >= 0)
  {
    close(server_fd_);
  }
}

void Server::setup_socket()
{
  server_fd_ = socket(AF_INET, SOCK_STREAM, 0);
  if (server_fd_ < 0)
  {
    throw std::runtime_error(std::string("socket failed: ") + std::strerror(errno));
  }

  int opt = 1;
  if (setsockopt(server_fd_, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt)) < 0)
  {
    throw std::runtime_error(std::string("setsockopt failed: ") + std::strerror(errno));
  }

  sockaddr_in addr{};
  addr.sin_family = AF_INET;
  addr.sin_addr.s_addr = INADDR_ANY;
  addr.sin_port = htons(port_);

  if (bind(server_fd_, reinterpret_cast<sockaddr *>(&addr), sizeof(addr)) < 0)
  {
    throw std::runtime_error(std::string("bind failed: ") + std::strerror(errno));
  }

  if (listen(server_fd_, 128) < 0)
  {
    throw std::runtime_error(std::string("listen failed: ") + std::strerror(errno));
  }
}

void Server::start()
{
  setup_socket();
  running_ = true;

  std::cout << "Vajra listening on port " << port_ << std::endl;

  while (running_)
  {
    sockaddr_in client_addr{};
    socklen_t client_len = sizeof(client_addr);

    int client_fd = accept(server_fd_, reinterpret_cast<sockaddr *>(&client_addr), &client_len);
    if (client_fd < 0)
    {
      if (!running_)
        break;
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

      std::cout.write(buffer, bytes_read);
      std::cout.flush();

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
}

void Server::stop()
{
  running_ = false;

  if (server_fd_ >= 0)
  {
    close(server_fd_);
    server_fd_ = -1;
  }
}
