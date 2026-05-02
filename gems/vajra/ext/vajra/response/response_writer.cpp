// Copyright Codevedas Inc. 2025-present
//
// This source code is licensed under the MIT license found in the
// LICENSE file in the root directory of this source tree.

#include "response_writer.hpp"

#include <cerrno>
#include <cstring>
#include <iostream>
#include <sys/socket.h>

namespace
{
#ifdef MSG_NOSIGNAL
  constexpr int kSendFlags = MSG_NOSIGNAL;
#else
  constexpr int kSendFlags = 0;
#endif
}

bool Vajra::response::ResponseWriter::send(int client_fd, const Response &response) const
{
  try
  {
    const std::string response_message = serializer_.serialize(response);
    return send_response_message(client_fd, response_message);
  }
  catch (const SerializationError &error)
  {
    log_serialization_error(error);
    return false;
  }
}

Vajra::response::Response Vajra::response::ResponseWriter::success_response() const
{
  return Response{
      Status{200, "OK"},
      {Header{"Content-Type", "text/plain"}},
      "OK"};
}

Vajra::response::Response Vajra::response::ResponseWriter::request_head_failure_response(
    Vajra::request::HeadFailureKind kind) const
{
  switch (kind)
  {
  case Vajra::request::HeadFailureKind::bad_request:
    return Response{
        Status{400, "Bad Request"},
        {Header{"Content-Type", "text/plain"}},
        "Bad Request"};
  case Vajra::request::HeadFailureKind::header_too_large:
    return Response{
        Status{431, "Request Header Fields Too Large"},
        {Header{"Content-Type", "text/plain"}},
        "Request Header Fields Too Large"};
  }

  return Response{
      Status{400, "Bad Request"},
      {Header{"Content-Type", "text/plain"}},
      "Bad Request"};
}

void Vajra::response::ResponseWriter::log_request_head_error(const Vajra::request::HeadError &error) const
{
  std::cerr << "request rejected (" << request_head_failure_label(error.kind()) << "): " << error.what()
            << std::endl;
}

void Vajra::response::ResponseWriter::log_serialization_error(const SerializationError &error) const
{
  std::cerr << "response serialization failed: " << error.what() << std::endl;
}

bool Vajra::response::ResponseWriter::send_response_message(int client_fd, const std::string &response_message) const
{
  suppress_sigpipe(client_fd);

  std::size_t bytes_sent = 0;
  while (bytes_sent < response_message.size())
  {
    const ssize_t sent = ::send(
        client_fd,
        response_message.data() + bytes_sent,
        response_message.size() - bytes_sent,
        kSendFlags);
    if (sent < 0)
    {
      if (errno == EINTR)
      {
        continue;
      }

      std::cerr << "send failed: " << std::strerror(errno) << std::endl;
      return false;
    }

    if (sent == 0)
    {
      std::cerr << "send failed: connection closed before response completed" << std::endl;
      return false;
    }

    bytes_sent += static_cast<std::size_t>(sent);
  }

  return true;
}

void Vajra::response::ResponseWriter::suppress_sigpipe(int client_fd) const
{
#ifdef SO_NOSIGPIPE
  int opt = 1;
  if (setsockopt(client_fd, SOL_SOCKET, SO_NOSIGPIPE, &opt, sizeof(opt)) < 0)
  {
    std::cerr << "setsockopt(SO_NOSIGPIPE) failed: " << std::strerror(errno) << std::endl;
  }
#else
  (void)client_fd;
#endif
}

const char *Vajra::response::ResponseWriter::request_head_failure_label(
    Vajra::request::HeadFailureKind kind) const
{
  switch (kind)
  {
  case Vajra::request::HeadFailureKind::bad_request:
    return "400 bad request";
  case Vajra::request::HeadFailureKind::header_too_large:
    return "431 request header fields too large";
  }

  return "request head failure";
}
