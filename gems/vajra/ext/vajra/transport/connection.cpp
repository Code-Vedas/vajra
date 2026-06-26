// Copyright Codevedas Inc. 2025-present
//
// This source code is licensed under the MIT license found in the
// LICENSE file in the root directory of this source tree.

#include "connection.hpp"

#include <cerrno>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

Vajra::transport::PlainConnection::PlainConnection(int client_fd) : client_fd_(client_fd)
{
}

int Vajra::transport::PlainConnection::fd() const
{
  return client_fd_;
}

bool Vajra::transport::PlainConnection::wait_readable(int timeout_seconds)
{
  pollfd descriptor{client_fd_, POLLIN | POLLHUP | POLLERR, 0};
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

ssize_t Vajra::transport::PlainConnection::read(char *buffer, std::size_t length)
{
  return recv(client_fd_, buffer, length, 0);
}

ssize_t Vajra::transport::PlainConnection::write(const char *buffer, std::size_t length)
{
#ifdef MSG_NOSIGNAL
  return send(client_fd_, buffer, length, MSG_NOSIGNAL);
#else
  return send(client_fd_, buffer, length, 0);
#endif
}

std::string Vajra::transport::PlainConnection::protocol() const
{
  return "http/1.1";
}

bool Vajra::transport::PlainConnection::tls() const
{
  return false;
}
