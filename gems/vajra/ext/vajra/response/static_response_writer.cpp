// Copyright Codevedas Inc. 2025-present
//
// This source code is licensed under the MIT license found in the
// LICENSE file in the root directory of this source tree.

#include "static_response_writer.hpp"

#include <cerrno>
#include <cstring>
#include <iostream>
#include <sys/socket.h>

namespace
{
  constexpr const char *kBadRequestResponse =
      "HTTP/1.1 400 Bad Request\r\n"
      "Content-Length: 11\r\n"
      "Content-Type: text/plain\r\n"
      "Connection: close\r\n"
      "\r\n"
      "Bad Request";
  constexpr const char *kRequestHeaderFieldsTooLargeResponse =
      "HTTP/1.1 431 Request Header Fields Too Large\r\n"
      "Content-Length: 31\r\n"
      "Content-Type: text/plain\r\n"
      "Connection: close\r\n"
      "\r\n"
      "Request Header Fields Too Large";
  constexpr const char *kSuccessResponse =
      "HTTP/1.1 200 OK\r\n"
      "Content-Length: 2\r\n"
      "Content-Type: text/plain\r\n"
      "Connection: close\r\n"
      "\r\n"
      "OK";

#ifdef MSG_NOSIGNAL
  constexpr int kSendFlags = MSG_NOSIGNAL;
#else
  constexpr int kSendFlags = 0;
#endif
}

bool Vajra::response::StaticResponseWriter::send_success_response(int client_fd) const
{
  return send_response_message(client_fd, kSuccessResponse);
}

void Vajra::response::StaticResponseWriter::send_request_head_failure_response(
    int client_fd,
    Vajra::request::HeadFailureKind kind) const
{
  (void)send_response_message(client_fd, request_head_failure_response(kind));
}

void Vajra::response::StaticResponseWriter::log_request_head_error(const Vajra::request::HeadError &error) const
{
  std::cerr << "request rejected (" << request_head_failure_label(error.kind()) << "): " << error.what()
            << std::endl;
}

bool Vajra::response::StaticResponseWriter::send_response_message(int client_fd, const char *response) const
{
  suppress_sigpipe(client_fd);

  const std::size_t response_length = std::strlen(response);
  std::size_t bytes_sent = 0;
  while (bytes_sent < response_length)
  {
    const ssize_t sent = send(client_fd, response + bytes_sent, response_length - bytes_sent, kSendFlags);
    if (sent < 0)
    {
      std::cerr << "send failed: " << std::strerror(errno) << std::endl;
      return false;
    }

    bytes_sent += static_cast<std::size_t>(sent);
  }

  return true;
}

void Vajra::response::StaticResponseWriter::suppress_sigpipe(int client_fd) const
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

const char *Vajra::response::StaticResponseWriter::request_head_failure_label(
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

const char *Vajra::response::StaticResponseWriter::request_head_failure_response(
    Vajra::request::HeadFailureKind kind) const
{
  switch (kind)
  {
  case Vajra::request::HeadFailureKind::bad_request:
    return kBadRequestResponse;
  case Vajra::request::HeadFailureKind::header_too_large:
    return kRequestHeaderFieldsTooLargeResponse;
  }

  return kBadRequestResponse;
}
