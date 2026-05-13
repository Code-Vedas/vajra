// Copyright Codevedas Inc. 2025-present
//
// This source code is licensed under the MIT license found in the
// LICENSE file in the root directory of this source tree.

#include "request_body_reader.hpp"

#include "http_field_utils.hpp"
#include "request_head_error.hpp"

#include <cerrno>
#include <cstdlib>
#include <limits>
#include <string>
#include <sys/socket.h>
#include <vector>

namespace
{
  enum class BodyFraming
  {
    none,
    content_length,
    chunked
  };

  struct BodyReadPlan
  {
    BodyFraming framing;
    std::size_t content_length = 0;
  };

  std::string bad_request_message_for_body_read()
  {
    return "request body framing is invalid";
  }

  [[noreturn]] void raise_invalid_body_read()
  {
    throw Vajra::request::bad_request_error(bad_request_message_for_body_read());
  }

  [[noreturn]] void raise_request_body_too_large()
  {
    throw Vajra::request::bad_request_error("request body exceeds maximum size");
  }

  [[noreturn]] void raise_request_body_metadata_too_large()
  {
    throw Vajra::request::bad_request_error("request body metadata exceeds maximum size");
  }

  [[noreturn]] void raise_incomplete_body_read()
  {
    throw Vajra::request::BodyReadIncompleteError();
  }

  std::size_t parse_decimal_content_length(const std::string &value)
  {
    const std::string normalized = Vajra::request::strip_http_whitespace(value);
    if (normalized.empty())
    {
      raise_invalid_body_read();
    }

    std::size_t content_length = 0;
    for (const char character : normalized)
    {
      if (character < '0' || character > '9')
      {
        raise_invalid_body_read();
      }

      const std::size_t digit = static_cast<std::size_t>(character - '0');
      if (content_length > (std::numeric_limits<std::size_t>::max() - digit) / 10)
      {
        raise_invalid_body_read();
      }

      content_length = content_length * 10 + digit;
    }

    return content_length;
  }

  std::vector<std::string> transfer_encoding_tokens(const std::string &value)
  {
    std::vector<std::string> tokens;
    std::size_t cursor = 0;

    while (cursor <= value.size())
    {
      const std::size_t delimiter = value.find(',', cursor);
      const std::string token =
          Vajra::request::strip_http_whitespace(value.substr(cursor, delimiter - cursor));
      if (token.empty())
      {
        raise_invalid_body_read();
      }

      tokens.push_back(token);
      if (delimiter == std::string::npos)
      {
        break;
      }

      cursor = delimiter + 1;
    }

    return tokens;
  }

  BodyReadPlan body_read_plan_for(const Vajra::request::ParsedRequest &request)
  {
    bool saw_content_length = false;
    bool saw_transfer_encoding = false;
    std::size_t content_length = 0;
    std::vector<std::string> transfer_encodings;

    for (const Vajra::request::ParsedHeader &header : request.headers)
    {
      if (Vajra::request::ascii_case_insensitive_equal(header.name, "Content-Length"))
      {
        if (saw_content_length)
        {
          raise_invalid_body_read();
        }

        content_length = parse_decimal_content_length(header.value);
        saw_content_length = true;
        continue;
      }

      if (Vajra::request::ascii_case_insensitive_equal(header.name, "Transfer-Encoding"))
      {
        if (saw_transfer_encoding)
        {
          raise_invalid_body_read();
        }

        transfer_encodings = transfer_encoding_tokens(header.value);
        saw_transfer_encoding = true;
      }
    }

    if (saw_content_length && saw_transfer_encoding)
    {
      raise_invalid_body_read();
    }

    if (saw_transfer_encoding)
    {
      if (transfer_encodings.size() != 1 ||
          !Vajra::request::ascii_case_insensitive_equal(transfer_encodings[0], "chunked"))
      {
        raise_invalid_body_read();
      }

      return BodyReadPlan{BodyFraming::chunked, 0};
    }

    if (!saw_content_length || content_length == 0)
    {
      return BodyReadPlan{BodyFraming::none, 0};
    }

    return BodyReadPlan{BodyFraming::content_length, content_length};
  }

  std::size_t unread_bytes(const std::string &buffer, std::size_t cursor)
  {
    return buffer.size() - cursor;
  }

  void compact_consumed_prefix(std::string &buffer, std::size_t &cursor)
  {
    if (cursor == 0)
    {
      return;
    }

    if (cursor >= buffer.size())
    {
      buffer.clear();
      cursor = 0;
      return;
    }

    if (cursor < 4096 && cursor * 2 < buffer.size())
    {
      return;
    }

    buffer.erase(0, cursor);
    cursor = 0;
  }

  std::string unread_suffix(std::string &buffer, std::size_t cursor)
  {
    if (cursor == 0)
    {
      return std::move(buffer);
    }

    return buffer.substr(cursor);
  }

  void append_socket_bytes(std::string &buffer, int client_fd)
  {
    char chunk[4096];

    for (;;)
    {
      const ssize_t bytes_read = recv(client_fd, chunk, sizeof(chunk), 0);
      if (bytes_read < 0)
      {
        if (errno == EINTR)
        {
          continue;
        }

        if (errno == EAGAIN || errno == EWOULDBLOCK)
        {
          raise_incomplete_body_read();
        }

        raise_invalid_body_read();
      }

      if (bytes_read == 0)
      {
        raise_incomplete_body_read();
      }

      buffer.append(chunk, static_cast<std::size_t>(bytes_read));
      return;
    }
  }

  void ensure_buffer_bytes(std::string &buffer, std::size_t cursor, std::size_t expected_size, int client_fd)
  {
    while (unread_bytes(buffer, cursor) < expected_size)
    {
      append_socket_bytes(buffer, client_fd);
    }
  }

  std::string read_line(std::string &buffer, std::size_t &cursor, int client_fd, std::size_t max_line_bytes)
  {
    while (true)
    {
      const std::size_t line_end = buffer.find("\r\n", cursor);
      if (line_end != std::string::npos)
      {
        const std::string line = buffer.substr(cursor, line_end - cursor);
        cursor = line_end + 2;
        compact_consumed_prefix(buffer, cursor);
        return line;
      }

      if (unread_bytes(buffer, cursor) >= max_line_bytes)
      {
        raise_request_body_metadata_too_large();
      }

      append_socket_bytes(buffer, client_fd);
    }
  }

  std::size_t parse_chunk_size(const std::string &line)
  {
    const std::size_t extension_delimiter = line.find(';');
    const std::string size_token = Vajra::request::strip_http_whitespace(line.substr(0, extension_delimiter));
    if (size_token.empty())
    {
      raise_invalid_body_read();
    }

    std::size_t chunk_size = 0;
    for (const char character : size_token)
    {
      std::size_t digit = 0;
      if (character >= '0' && character <= '9')
      {
        digit = static_cast<std::size_t>(character - '0');
      }
      else if (character >= 'a' && character <= 'f')
      {
        digit = static_cast<std::size_t>(character - 'a' + 10);
      }
      else if (character >= 'A' && character <= 'F')
      {
        digit = static_cast<std::size_t>(character - 'A' + 10);
      }
      else
      {
        raise_invalid_body_read();
      }

      if (chunk_size > (std::numeric_limits<std::size_t>::max() - digit) / 16)
      {
        raise_invalid_body_read();
      }

      chunk_size = chunk_size * 16 + digit;
    }

    return chunk_size;
  }

  void validate_trailer_line(const std::string &line)
  {
    const std::size_t delimiter = line.find(':');
    if (delimiter == std::string::npos || delimiter == 0)
    {
      raise_invalid_body_read();
    }

    const std::string name = line.substr(0, delimiter);
    if (name.find_first_of(" \t") != std::string::npos)
    {
      raise_invalid_body_read();
    }
  }

  void consume_trailers(std::string &buffer, std::size_t &cursor, int client_fd, std::size_t max_trailer_line_bytes)
  {
    while (true)
    {
      const std::string trailer_line = read_line(buffer, cursor, client_fd, max_trailer_line_bytes);
      if (trailer_line.empty())
      {
        return;
      }

      validate_trailer_line(trailer_line);
    }
  }

  Vajra::request::BodyReadResult read_content_length_body(
      int client_fd,
      std::size_t content_length,
      std::string buffered_bytes,
      std::size_t max_request_body_bytes)
  {
    if (content_length > max_request_body_bytes)
    {
      raise_request_body_too_large();
    }

    const std::size_t body_start = 0;
    ensure_buffer_bytes(buffered_bytes, body_start, content_length, client_fd);
    return Vajra::request::BodyReadResult{
        buffered_bytes.substr(body_start, content_length),
        buffered_bytes.substr(body_start + content_length)};
  }

  Vajra::request::BodyReadResult read_chunked_body(
      int client_fd,
      std::string buffered_bytes,
      std::size_t max_request_body_bytes,
      std::size_t max_chunk_line_bytes,
      std::size_t max_trailer_line_bytes)
  {
    std::string body;
    std::size_t cursor = 0;

    while (true)
    {
      const std::size_t chunk_size = parse_chunk_size(
          read_line(buffered_bytes, cursor, client_fd, max_chunk_line_bytes));
      if (chunk_size == 0)
      {
        consume_trailers(buffered_bytes, cursor, client_fd, max_trailer_line_bytes);
        return Vajra::request::BodyReadResult{body, unread_suffix(buffered_bytes, cursor)};
      }

      if (chunk_size > max_request_body_bytes - body.size())
      {
        raise_request_body_too_large();
      }

      ensure_buffer_bytes(buffered_bytes, cursor, chunk_size + 2, client_fd);
      body.append(buffered_bytes.data() + cursor, chunk_size);
      cursor += chunk_size;
      if (buffered_bytes[cursor] != '\r' || buffered_bytes[cursor + 1] != '\n')
      {
        raise_invalid_body_read();
      }

      cursor += 2;
      compact_consumed_prefix(buffered_bytes, cursor);
    }
  }
}

Vajra::request::BodyReadResult Vajra::request::RequestBodyReader::read(
    int client_fd,
    const ParsedRequest &request,
    std::string buffered_bytes) const
{
  const BodyReadPlan plan = body_read_plan_for(request);

  switch (plan.framing)
  {
    case BodyFraming::none:
      return BodyReadResult{"", std::move(buffered_bytes)};
    case BodyFraming::content_length:
      return read_content_length_body(
          client_fd,
          plan.content_length,
          std::move(buffered_bytes),
          max_request_body_bytes_);
    case BodyFraming::chunked:
      return read_chunked_body(
          client_fd,
          std::move(buffered_bytes),
          max_request_body_bytes_,
          max_chunk_line_bytes_,
          max_trailer_line_bytes_);
  }

  raise_invalid_body_read();
}
