// Copyright Codevedas Inc. 2025-present
//
// This source code is licensed under the MIT license found in the
// LICENSE file in the root directory of this source tree.

#include "request_head_reader.hpp"

#include <cerrno>
#include <cstring>
#include <poll.h>
#include <sys/socket.h>
#include <utility>

namespace
{
  constexpr const char *kHeaderBoundary = "\r\n\r\n";
  constexpr std::size_t kHeaderBoundaryLength = 4;

  bool peer_closed_recv_error(int error_number)
  {
    return error_number == ECONNRESET || error_number == ECONNABORTED || error_number == ENOTCONN;
  }
}

Vajra::request::HeadReader::HeadReader(std::size_t max_request_head_bytes, int continuation_timeout_seconds)
    : request_head_size_validator_(max_request_head_bytes),
      continuation_timeout_seconds_(continuation_timeout_seconds)
{
}

Vajra::request::HeadReadResult Vajra::request::HeadReader::read(
    int client_fd,
    std::string buffered_bytes,
    int initial_timeout_seconds) const
{
  int next_timeout_seconds = buffered_bytes.empty() ? initial_timeout_seconds : continuation_timeout_seconds_;

  char buffer[4096];
  std::string request_head = std::move(buffered_bytes);
  std::size_t next_header_boundary_search_start = 0;

  while (true)
  {
    const std::size_t header_boundary = request_head.find(kHeaderBoundary, next_header_boundary_search_start);
    if (header_boundary != std::string::npos)
    {
      const std::size_t request_head_bytes = header_boundary + kHeaderBoundaryLength;
      request_head_size_validator_.validate(request_head_bytes);

      const std::string trailing_bytes = request_head.substr(request_head_bytes);
      request_head.resize(request_head_bytes);
      return HeadReadResult{true, false, request_head, trailing_bytes};
    }

    request_head_size_validator_.validate(request_head.size());
    if (request_head.size() >= kHeaderBoundaryLength - 1)
    {
      next_header_boundary_search_start = request_head.size() - (kHeaderBoundaryLength - 1);
    }

    if (!wait_for_readable(client_fd, next_timeout_seconds))
    {
      return HeadReadResult{false, false, request_head, ""};
    }

    const ssize_t bytes_read = recv(client_fd, buffer, sizeof(buffer), MSG_DONTWAIT);
    if (bytes_read < 0)
    {
      if (errno == EINTR)
      {
        continue;
      }

      if (errno == EAGAIN || errno == EWOULDBLOCK)
      {
        continue;
      }
      if (peer_closed_recv_error(errno))
      {
        return HeadReadResult{false, true, request_head, ""};
      }

      return HeadReadResult{false, true, request_head, ""};
    }

    if (bytes_read == 0)
    {
      return HeadReadResult{false, true, request_head, ""};
    }
    request_head.append(buffer, bytes_read);
    next_timeout_seconds = continuation_timeout_seconds_;
  }
}

bool Vajra::request::HeadReader::wait_for_readable(int client_fd, int timeout_seconds) const
{
  pollfd descriptor{client_fd, POLLIN | POLLHUP | POLLERR, 0};
  const int timeout_milliseconds = timeout_seconds <= 0 ? 0 : timeout_seconds * 1000;

  for (;;)
  {
    const int poll_result = poll(&descriptor, 1, timeout_milliseconds);
    if (poll_result > 0)
    {
      return (descriptor.revents & (POLLIN | POLLHUP | POLLERR)) != 0;
    }
    if (poll_result == 0)
    {
      return false;
    }
    if (errno != EINTR)
    {
      return false;
    }
  }
}
