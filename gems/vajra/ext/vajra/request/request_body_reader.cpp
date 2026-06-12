// Copyright Codevedas Inc. 2025-present
//
// This source code is licensed under the MIT license found in the
// LICENSE file in the root directory of this source tree.

#include "request_body_reader.hpp"

#include "http_field_utils.hpp"
#include "request_head_error.hpp"
#include "request_executor.hpp"

#include <algorithm>
#include <chrono>
#include <cerrno>
#include <cstring>
#include <cstdlib>
#include <functional>
#include <limits>
#include <string>
#include <sys/socket.h>
#include <utility>
#include <vector>

namespace
{
  constexpr std::size_t kRequestBodyReadChunkBytes = 4096; // Traditional 4KB buffer
  using BodyReadDeadline = std::chrono::steady_clock::time_point;

  std::string bad_request_message_for_body_read()
  {
    return "request body framing is invalid";
  }

  [[noreturn]] void raise_invalid_body_read()
  {
    throw Vajra::request::bad_request_error(bad_request_message_for_body_read());
  }

  [[noreturn]] void raise_body_metadata_too_large_error()
  {
    throw Vajra::request::HeadError(Vajra::request::HeadFailureKind::bad_request, "request body metadata exceeds maximum size");
  }

  [[noreturn]] void raise_body_too_large_error()
  {
    throw Vajra::request::HeadError(Vajra::request::HeadFailureKind::bad_request, "request body exceeds maximum size");
  }

  std::size_t unread_bytes(const std::string &buffer, std::size_t cursor)
  {
    if (cursor >= buffer.size())
    {
      return 0;
    }
    return buffer.size() - cursor;
  }

  int remaining_wait_seconds(BodyReadDeadline deadline)
  {
    const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(
                               deadline - std::chrono::steady_clock::now())
                               .count();
    if (remaining <= 0)
    {
      throw Vajra::request::RequestTimeoutError("request body read timed out");
    }
    return static_cast<int>((remaining + 999) / 1000);
  }

  void wait_for_body_bytes(Vajra::transport::Connection &connection, BodyReadDeadline deadline)
  {
    if (connection.fd() < 0)
    {
      throw Vajra::request::BodyReadIncompleteError();
    }
    if (!connection.wait_readable(remaining_wait_seconds(deadline)))
    {
      throw Vajra::request::RequestTimeoutError("request body read timed out");
    }
  }

  bool ascii_hex_digit(char character)
  {
    return (character >= '0' && character <= '9') ||
           (character >= 'A' && character <= 'F') ||
           (character >= 'a' && character <= 'f');
  }

  std::size_t hex_digit_value(char character)
  {
    if (character >= '0' && character <= '9')
    {
      return static_cast<std::size_t>(character - '0');
    }
    if (character >= 'A' && character <= 'F')
    {
      return static_cast<std::size_t>(character - 'A' + 10);
    }
    return static_cast<std::size_t>(character - 'a' + 10);
  }

  std::size_t strict_chunk_size_value(const std::string &size_line)
  {
    const std::size_t extension_start = size_line.find(';');
    const std::string size_part = extension_start == std::string::npos
                                      ? size_line
                                      : size_line.substr(0, extension_start);
    if (size_part.empty())
    {
      raise_invalid_body_read();
    }

    std::size_t chunk_size = 0;
    for (const char character : size_part)
    {
      if (!ascii_hex_digit(character))
      {
        raise_invalid_body_read();
      }
      const std::size_t digit = hex_digit_value(character);
      if (chunk_size > (std::numeric_limits<std::size_t>::max() - digit) / 16)
      {
        raise_invalid_body_read();
      }
      chunk_size = chunk_size * 16 + digit;
    }
    return chunk_size;
  }

  std::size_t strict_content_length_value(const std::string &value)
  {
    const std::string stripped = Vajra::request::strip_http_whitespace(value);
    if (stripped.empty())
    {
      throw Vajra::request::bad_request_error("invalid Content-Length header");
    }

    std::size_t length = 0;
    for (const char character : stripped)
    {
      if (character < '0' || character > '9')
      {
        throw Vajra::request::bad_request_error("invalid Content-Length header");
      }
      const std::size_t digit = static_cast<std::size_t>(character - '0');
      if (length > (std::numeric_limits<std::size_t>::max() - digit) / 10)
      {
        throw Vajra::request::bad_request_error("invalid Content-Length header");
      }
      length = length * 10 + digit;
    }
    return length;
  }

  bool transfer_encoding_is_supported_chunked(const std::string &value)
  {
    bool saw_token = false;
    bool saw_chunked = false;
    std::size_t cursor = 0;
    while (cursor <= value.size())
    {
      const std::size_t comma = value.find(',', cursor);
      const std::string token = Vajra::request::strip_http_whitespace(
          value.substr(cursor, comma == std::string::npos ? std::string::npos : comma - cursor));
      if (token.empty())
      {
        throw Vajra::request::bad_request_error("unsupported Transfer-Encoding header");
      }
      saw_token = true;
      if (Vajra::request::ascii_case_insensitive_equal(token, "chunked"))
      {
        if (saw_chunked || comma != std::string::npos)
        {
          throw Vajra::request::bad_request_error("unsupported Transfer-Encoding header");
        }
        saw_chunked = true;
      }
      else
      {
        throw Vajra::request::bad_request_error("unsupported Transfer-Encoding header");
      }
      if (comma == std::string::npos)
      {
        break;
      }
      cursor = comma + 1;
    }
    return saw_token && saw_chunked;
  }

  void append_socket_bytes(std::string &buffer, Vajra::transport::Connection &connection, BodyReadDeadline deadline)
  {
    wait_for_body_bytes(connection, deadline);
    char chunk[kRequestBodyReadChunkBytes];
    const ssize_t bytes_read = connection.read(chunk, sizeof(chunk));
    if (bytes_read < 0)
    {
      if (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK)
      {
        return;
      }
      throw Vajra::request::BodyReadIncompleteError();
    }

    if (bytes_read == 0)
    {
      throw Vajra::request::BodyReadIncompleteError();
    }

    buffer.append(chunk, static_cast<std::size_t>(bytes_read));
  }

  void ensure_buffer_bytes(
      std::string &buffer,
      std::size_t cursor,
      std::size_t needed,
      Vajra::transport::Connection &connection,
      BodyReadDeadline deadline)
  {
    while (unread_bytes(buffer, cursor) < needed)
    {
      append_socket_bytes(buffer, connection, deadline);
    }
  }

  std::size_t parse_chunk_size(
      std::string &buffer,
      std::size_t &cursor,
      Vajra::transport::Connection &connection,
      std::size_t max_line_bytes,
      BodyReadDeadline deadline)
  {
    std::size_t line_end = std::string::npos;
    while (line_end == std::string::npos)
    {
      line_end = buffer.find("\r\n", cursor);
      if (line_end == std::string::npos)
      {
        if (unread_bytes(buffer, cursor) > max_line_bytes)
        {
          raise_body_metadata_too_large_error();
        }
        append_socket_bytes(buffer, connection, deadline);
      }
    }

    const std::string size_line = buffer.substr(cursor, line_end - cursor);
    cursor = line_end + 2;
    return strict_chunk_size_value(size_line);
  }

  void emit_body_chunk(const Vajra::request::BodyChunkCallback &on_body_chunk, const char *data, std::size_t length)
  {
    if (length > 0 && on_body_chunk)
    {
      on_body_chunk(data, length);
    }
  }

  void compact_consumed_prefix(std::string &buffer, std::size_t &cursor)
  {
    if (cursor > 16 * 1024)
    {
      buffer.erase(0, cursor);
      cursor = 0;
    }
  }

  Vajra::request::BodyReadResult read_chunked_body(
      Vajra::transport::Connection &connection,
      std::string buffered_bytes,
      const Vajra::request::BodyChunkCallback &on_body_chunk,
      std::size_t max_request_body_bytes,
      std::size_t max_chunk_line_bytes,
      std::size_t max_trailer_line_bytes,
      BodyReadDeadline deadline)
  {
    std::size_t cursor = 0;
    std::size_t total_body_bytes = 0;

    while (true)
    {
      const std::size_t chunk_size = parse_chunk_size(buffered_bytes, cursor, connection, max_chunk_line_bytes, deadline);
      if (chunk_size == 0)
      {
        for (;;)
        {
          std::size_t trailer_line_end = buffered_bytes.find("\r\n", cursor);
          while (trailer_line_end == std::string::npos)
          {
            if (unread_bytes(buffered_bytes, cursor) > max_trailer_line_bytes)
            {
              raise_body_metadata_too_large_error();
            }
            append_socket_bytes(buffered_bytes, connection, deadline);
            trailer_line_end = buffered_bytes.find("\r\n", cursor);
          }

          if (trailer_line_end == cursor)
          {
            return Vajra::request::BodyReadResult{"", buffered_bytes.substr(trailer_line_end + 2)};
          }

          if (trailer_line_end - cursor > max_trailer_line_bytes)
          {
            raise_body_metadata_too_large_error();
          }
          cursor = trailer_line_end + 2;
        }
      }

      if (total_body_bytes + chunk_size > max_request_body_bytes)
      {
        raise_body_too_large_error();
      }

      std::size_t remaining_chunk_bytes = chunk_size;
      while (remaining_chunk_bytes > 0)
      {
        if (unread_bytes(buffered_bytes, cursor) == 0)
        {
          append_socket_bytes(buffered_bytes, connection, deadline);
        }

        const std::size_t available_body_bytes = std::min(unread_bytes(buffered_bytes, cursor), remaining_chunk_bytes);
        emit_body_chunk(on_body_chunk, buffered_bytes.data() + cursor, available_body_bytes);
        cursor += available_body_bytes;
        remaining_chunk_bytes -= available_body_bytes;
        total_body_bytes += available_body_bytes;
        compact_consumed_prefix(buffered_bytes, cursor);
      }

      ensure_buffer_bytes(buffered_bytes, cursor, 2, connection, deadline);
      if (buffered_bytes[cursor] != '\r' || buffered_bytes[cursor + 1] != '\n')
      {
        raise_invalid_body_read();
      }
      cursor += 2;
      compact_consumed_prefix(buffered_bytes, cursor);
    }
  }

  Vajra::request::BodyReadResult read_content_length_body(
      Vajra::transport::Connection &connection,
      const Vajra::request::BodyReadPlan &plan,
      std::string buffered_bytes,
      const Vajra::request::BodyChunkCallback &on_body_chunk,
      std::size_t max_request_body_bytes,
      BodyReadDeadline deadline)
  {
    if (plan.content_length > max_request_body_bytes)
    {
      raise_body_too_large_error();
    }

    std::size_t remaining = plan.content_length;
    std::size_t cursor = 0;

    if (!buffered_bytes.empty())
    {
      const std::size_t buffered_body_bytes = std::min(buffered_bytes.size(), remaining);
      emit_body_chunk(on_body_chunk, buffered_bytes.data(), buffered_body_bytes);
      cursor = buffered_body_bytes;
      remaining -= buffered_body_bytes;
      if (remaining == 0)
      {
        return Vajra::request::BodyReadResult{"", buffered_bytes.substr(cursor)};
      }
    }

    char chunk[kRequestBodyReadChunkBytes];
    while (remaining > 0)
    {
      wait_for_body_bytes(connection, deadline);
      const std::size_t bytes_to_read = std::min<std::size_t>(sizeof(chunk), remaining);
      const ssize_t bytes_read = connection.read(chunk, bytes_to_read);
      if (bytes_read < 0)
      {
        if (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK)
        {
          continue;
        }
        throw Vajra::request::BodyReadIncompleteError();
      }

      if (bytes_read == 0)
      {
        throw Vajra::request::BodyReadIncompleteError();
      }

      emit_body_chunk(on_body_chunk, chunk, static_cast<std::size_t>(bytes_read));
      remaining -= static_cast<std::size_t>(bytes_read);
    }

    return Vajra::request::BodyReadResult{"", ""};
  }
}

Vajra::request::BodyReadResult Vajra::request::RequestBodyReader::stream_read(
    int client_fd,
    const ParsedRequest &request,
    const BodyChunkCallback &on_body_chunk,
    std::string buffered_bytes) const
{
  Vajra::transport::PlainConnection connection(client_fd);
  return stream_read(connection, request, on_body_chunk, std::move(buffered_bytes));
}

Vajra::request::BodyReadResult Vajra::request::RequestBodyReader::stream_read(
    Vajra::transport::Connection &connection,
    const ParsedRequest &request,
    const BodyChunkCallback &on_body_chunk,
    std::string buffered_bytes) const
{
  const BodyReadPlan plan = plan_for(request);
  const BodyReadDeadline deadline = std::chrono::steady_clock::now() +
                                    std::chrono::seconds(request_body_timeout_seconds_);

  if (plan.framing == BodyFraming::none)
  {
    return BodyReadResult{"", std::move(buffered_bytes)};
  }
  else if (plan.framing == BodyFraming::chunked)
  {
    return read_chunked_body(connection, std::move(buffered_bytes), on_body_chunk, max_request_body_bytes_, max_chunk_line_bytes_, max_trailer_line_bytes_, deadline);
  }
  else
  {
    return read_content_length_body(connection, plan, std::move(buffered_bytes), on_body_chunk, max_request_body_bytes_, deadline);
  }
}

Vajra::request::BodyReadResult Vajra::request::RequestBodyReader::read(
    int client_fd,
    const ParsedRequest &request,
    std::string buffered_bytes) const
{
  Vajra::transport::PlainConnection connection(client_fd);
  return read(connection, request, std::move(buffered_bytes));
}

Vajra::request::BodyReadResult Vajra::request::RequestBodyReader::read(
    Vajra::transport::Connection &connection,
    const ParsedRequest &request,
    std::string buffered_bytes) const
{
  std::string body;
  const BodyReadPlan plan = plan_for(request);
  if (plan.framing == BodyFraming::content_length)
  {
    body.reserve(plan.content_length);
  }

  const BodyReadResult result = stream_read(
      connection,
      request,
      [&body](const char *data, std::size_t length)
      {
        body.append(data, length);
      },
      std::move(buffered_bytes));

  return BodyReadResult{std::move(body), result.remaining_buffered_bytes};
}

Vajra::request::BodyReadPlan Vajra::request::RequestBodyReader::plan_for(const ParsedRequest &request) const
{
  BodyReadPlan plan{BodyFraming::none, 0};
  bool saw_content_length = false;
  bool saw_transfer_encoding = false;

  for (const auto &header : request.headers)
  {
    if (ascii_case_insensitive_equal(header.name, "Transfer-Encoding"))
    {
      if (saw_transfer_encoding)
      {
        throw bad_request_error("unsupported Transfer-Encoding header");
      }
      saw_transfer_encoding = true;
      if (saw_content_length)
      {
        throw bad_request_error("conflicting request body framing");
      }
      if (transfer_encoding_is_supported_chunked(header.value))
      {
        plan.framing = BodyFraming::chunked;
        return plan;
      }
    }

    if (ascii_case_insensitive_equal(header.name, "Content-Length"))
    {
      if (saw_content_length)
      {
        throw bad_request_error("invalid Content-Length header");
      }
      saw_content_length = true;
      if (saw_transfer_encoding)
      {
        throw bad_request_error("conflicting request body framing");
      }
      plan.content_length = strict_content_length_value(header.value);
      plan.framing = BodyFraming::content_length;
    }
  }

  return plan;
}

bool Vajra::request::RequestBodyReader::can_read_without_streaming(
    const BodyReadPlan &plan,
    const std::string &buffered_bytes) const
{
  return plan.framing == BodyFraming::content_length &&
         plan.content_length <= max_request_body_bytes_ &&
         (plan.content_length <= kDefaultDirectRequestBodyBytes ||
          buffered_bytes.size() >= plan.content_length);
}
