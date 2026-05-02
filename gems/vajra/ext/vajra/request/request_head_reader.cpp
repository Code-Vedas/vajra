// Copyright Codevedas Inc. 2025-present
//
// This source code is licensed under the MIT license found in the
// LICENSE file in the root directory of this source tree.

#include "request_head_reader.hpp"

#include <cerrno>
#include <cstring>
#include <iostream>
#include <sys/socket.h>
#include <sys/time.h>

namespace
{
  constexpr const char *kHeaderBoundary = "\r\n\r\n";
  constexpr int kRequestHeadReadTimeoutSeconds = 5;
}

Vajra::request::HeadReader::HeadReader(std::size_t max_request_head_bytes)
    : request_head_size_validator_(max_request_head_bytes)
{
}

Vajra::request::HeadReadResult Vajra::request::HeadReader::read(int client_fd) const
{
  if (!configure_read_timeout(client_fd))
  {
    return HeadReadResult{false, ""};
  }

  char buffer[4096];
  std::string request_head;

  while (true)
  {
    const ssize_t bytes_read = recv(client_fd, buffer, sizeof(buffer), 0);
    if (bytes_read < 0)
    {
      if (errno == EINTR)
      {
        continue;
      }

      if (errno == EAGAIN || errno == EWOULDBLOCK)
      {
        return HeadReadResult{false, request_head};
      }

      std::cerr << "recv failed: " << std::strerror(errno) << std::endl;
      return HeadReadResult{false, request_head};
    }

    if (bytes_read == 0)
    {
      return HeadReadResult{false, request_head};
    }

    request_head.append(buffer, bytes_read);
    const std::size_t header_boundary = request_head.find(kHeaderBoundary);
    if (header_boundary != std::string::npos)
    {
      const std::size_t request_head_bytes = header_boundary + std::strlen(kHeaderBoundary);
      request_head_size_validator_.validate(request_head_bytes);
      request_head.resize(request_head_bytes);
      return HeadReadResult{true, request_head};
    }

    request_head_size_validator_.validate(request_head.size());
  }
}

bool Vajra::request::HeadReader::configure_read_timeout(int client_fd) const
{
  timeval timeout{};
  timeout.tv_sec = kRequestHeadReadTimeoutSeconds;
  timeout.tv_usec = 0;
  if (setsockopt(client_fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout)) < 0)
  {
    std::cerr << "setsockopt(SO_RCVTIMEO) failed: " << std::strerror(errno) << std::endl;
    return false;
  }

  return true;
}
