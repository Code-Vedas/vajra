// Copyright Codevedas Inc. 2025-present
//
// This source code is licensed under the MIT license found in the
// LICENSE file in the root directory of this source tree.

#include "connection.hpp"

Vajra::transport::PlainConnection::PlainConnection(platform::SocketHandle client_fd) : client_fd_(client_fd)
{
}

Vajra::platform::SocketHandle Vajra::transport::PlainConnection::fd() const
{
  return client_fd_;
}

bool Vajra::transport::PlainConnection::wait_readable(int timeout_seconds)
{
  const int timeout_milliseconds = timeout_seconds <= 0 ? 0 : timeout_seconds * 1000;
  return platform::wait_socket(client_fd_, platform::WaitEvent::read, timeout_milliseconds);
}

Vajra::platform::SignedSize Vajra::transport::PlainConnection::read(char *buffer, std::size_t length)
{
  return platform::receive_socket(client_fd_, buffer, length);
}

Vajra::platform::SignedSize Vajra::transport::PlainConnection::write(const char *buffer, std::size_t length)
{
  return platform::send_socket(client_fd_, buffer, length);
}

std::string Vajra::transport::PlainConnection::protocol() const
{
  return "http/1.1";
}

bool Vajra::transport::PlainConnection::tls() const
{
  return false;
}
