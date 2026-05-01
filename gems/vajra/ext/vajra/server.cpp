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
#include <string>
#include <sys/socket.h>
#include <unistd.h>

namespace
{
  constexpr const char *kHeaderBoundary = "\r\n\r\n";

  std::runtime_error startup_error(const char *stage, int port, int error_number)
  {
    return std::runtime_error(
        std::string("listener ") + stage + " failed for port " + std::to_string(port) + ": " +
        std::strerror(error_number));
  }

  std::runtime_error parse_error(const std::string &message)
  {
    return std::runtime_error("request parsing failed: " + message);
  }

  std::string strip_leading_header_whitespace(std::string value)
  {
    const std::size_t first_non_whitespace = value.find_first_not_of(" \t");
    if (first_non_whitespace == std::string::npos)
    {
      return "";
    }

    return value.substr(first_non_whitespace);
  }
}

ParsedRequest parse_request_head(const std::string &request_head)
{
  const std::size_t request_line_end = request_head.find("\r\n");
  if (request_line_end == std::string::npos)
  {
    throw parse_error("missing request line terminator");
  }

  const std::string request_line = request_head.substr(0, request_line_end);
  const std::size_t first_space = request_line.find(' ');
  if (first_space == std::string::npos || first_space == 0)
  {
    throw parse_error("invalid request line");
  }

  const std::size_t second_space = request_line.find(' ', first_space + 1);
  if (second_space == std::string::npos || second_space == first_space + 1)
  {
    throw parse_error("invalid request line");
  }

  if (request_line.find(' ', second_space + 1) != std::string::npos)
  {
    throw parse_error("invalid request line");
  }

  ParsedRequest parsed_request{
      ParsedRequestLine{
          request_line.substr(0, first_space),
          request_line.substr(first_space + 1, second_space - first_space - 1),
          request_line.substr(second_space + 1)},
      {}};

  if (parsed_request.request_line.version != "HTTP/1.1")
  {
    throw parse_error("invalid HTTP version");
  }

  std::size_t cursor = request_line_end + 2;
  while (cursor < request_head.size())
  {
    const std::size_t line_end = request_head.find("\r\n", cursor);
    if (line_end == std::string::npos)
    {
      throw parse_error("unterminated header line");
    }

    if (line_end == cursor)
    {
      return parsed_request;
    }

    const std::string header_line = request_head.substr(cursor, line_end - cursor);
    const std::size_t delimiter = header_line.find(':');
    if (delimiter == std::string::npos || delimiter == 0)
    {
      throw parse_error("invalid header line");
    }

    const std::string header_name = header_line.substr(0, delimiter);
    if (header_name.find_first_of(" \t") != std::string::npos)
    {
      throw parse_error("invalid header name");
    }

    parsed_request.headers.push_back(
        ParsedHeader{header_name, strip_leading_header_whitespace(header_line.substr(delimiter + 1))});
    cursor = line_end + 2;
  }

  throw parse_error("missing header terminator");
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
  if (stop_requested_.load())
  {
    return;
  }

  setup_socket();

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
    bool request_complete = false;
    std::string request_head;
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

      request_head.append(buffer, bytes_read);
      const std::size_t header_boundary = request_head.find(kHeaderBoundary);
      if (header_boundary != std::string::npos)
      {
        request_head.resize(header_boundary + std::strlen(kHeaderBoundary));
        request_complete = true;
        break;
      }
    }

    if (!request_complete)
    {
      close(client_fd);
      continue;
    }

    try
    {
      (void)parse_request_head(request_head);
    }
    catch (const std::exception &error)
    {
      std::cerr << error.what() << std::endl;
      close(client_fd);
      continue;
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
