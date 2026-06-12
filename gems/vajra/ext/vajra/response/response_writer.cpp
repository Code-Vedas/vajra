// Copyright Codevedas Inc. 2025-present
//
// This source code is licensed under the MIT license found in the
// LICENSE file in the root directory of this source tree.

#include "response_writer.hpp"

#include "runtime/runtime_state.hpp"

#include <chrono>
#include <array>
#include <cerrno>
#include <cstring>
#include <iostream>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/socket.h>

namespace
{
}

void Vajra::response::ResponseWriter::prepare_client_socket(int client_fd)
{
  int opt = 1;
#ifdef TCP_NODELAY
  (void)setsockopt(client_fd, IPPROTO_TCP, TCP_NODELAY, &opt, sizeof(opt));
#endif
#ifdef SO_NOSIGPIPE
  (void)setsockopt(client_fd, SOL_SOCKET, SO_NOSIGPIPE, &opt, sizeof(opt));
#endif
  (void)client_fd;
}

bool Vajra::response::ResponseWriter::send(int client_fd, const Response &response) const
{
  prepare_client_socket(client_fd);
  Vajra::transport::PlainConnection connection(client_fd);
  return send(connection, response);
}

bool Vajra::response::ResponseWriter::send(Vajra::transport::Connection &connection, const Response &response) const
{
  return send(connection, response, false);
}

bool Vajra::response::ResponseWriter::send(Vajra::transport::Connection &connection, const Response &response, bool suppress_body) const
{
  const auto started_at = std::chrono::steady_clock::now();
  try
  {
    bool sent = send_response_message(connection, serializer_.serialize_head(response));
    if (sent && !suppress_body && !response_body_empty(response))
    {
      if (response_has_body_chunks(response))
      {
        for (const std::string &chunk : response.body_chunks)
        {
          if (!send_response_message(connection, chunk))
          {
            sent = false;
            break;
          }
        }
      }
      else if (response_has_body_file(response))
      {
        if (std::fseek(response.body_file->file, 0, SEEK_SET) != 0)
        {
          return false;
        }
        std::array<char, 16 * 1024> buffer;
        for (;;)
        {
          const std::size_t read = std::fread(buffer.data(), 1, buffer.size(), response.body_file->file);
          if (read > 0 && !send_response_bytes(connection, buffer.data(), read))
          {
            sent = false;
            break;
          }
          if (read < buffer.size())
          {
            if (std::ferror(response.body_file->file) != 0)
            {
              sent = false;
            }
            break;
          }
        }
      }
      else
      {
        sent = send_response_message(connection, response.body);
      }
    }
    Vajra::runtime::note_worker_response_write_time(
        std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::now() - started_at)
            .count());
    return sent;
  }
  catch (const SerializationError &error)
  {
    log_serialization_error(error);
    return false;
  }
}

Vajra::response::Response Vajra::response::ResponseWriter::success_response(
    ConnectionBehavior connection_behavior) const
{
  return Response{
      Status{200, "OK"},
      {Header{"Content-Type", "text/plain"}},
      "OK",
      connection_behavior};
}

Vajra::response::Response Vajra::response::ResponseWriter::internal_server_error_response() const
{
  return Response{
      Status{500, "Internal Server Error"},
      {Header{"Content-Type", "text/plain"}},
      "Internal Server Error",
      ConnectionBehavior::close};
}

Vajra::response::Response Vajra::response::ResponseWriter::queue_capacity_response() const
{
  return Response{
      Status{503, "Service Unavailable"},
      {Header{"Content-Type", "text/plain"}},
      "Service Unavailable",
      ConnectionBehavior::close};
}

Vajra::response::Response Vajra::response::ResponseWriter::request_timeout_response() const
{
  return Response{
      Status{408, "Request Timeout"},
      {Header{"Content-Type", "text/plain"}},
      "Request Timeout",
      ConnectionBehavior::close};
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
        "Bad Request",
        ConnectionBehavior::close};
  case Vajra::request::HeadFailureKind::header_too_large:
    return Response{
        Status{431, "Request Header Fields Too Large"},
        {Header{"Content-Type", "text/plain"}},
        "Request Header Fields Too Large",
        ConnectionBehavior::close};
  }

  return Response{
      Status{400, "Bad Request"},
      {Header{"Content-Type", "text/plain"}},
      "Bad Request",
      ConnectionBehavior::close};
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
  prepare_client_socket(client_fd);
  Vajra::transport::PlainConnection connection(client_fd);
  return send_response_message(connection, response_message);
}

bool Vajra::response::ResponseWriter::send_response_message(
    Vajra::transport::Connection &connection,
    const std::string &response_message) const
{
  return send_response_bytes(connection, response_message.data(), response_message.size());
}

bool Vajra::response::ResponseWriter::send_response_bytes(
    Vajra::transport::Connection &connection,
    const char *data,
    std::size_t length) const
{
  std::size_t bytes_sent = 0;
  while (bytes_sent < length)
  {
    const ssize_t sent = connection.write(
        data + bytes_sent,
        length - bytes_sent);
    if (sent < 0)
    {
      if (errno == EINTR)
      {
        continue;
      }

      if (errno != EPIPE && errno != ECONNRESET && errno != ECONNABORTED)
      {
        std::cerr << "send failed: " << std::strerror(errno) << std::endl;
      }
      return false;
    }

    if (sent == 0)
    {
      return false;
    }

    bytes_sent += static_cast<std::size_t>(sent);
  }

  return true;
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
