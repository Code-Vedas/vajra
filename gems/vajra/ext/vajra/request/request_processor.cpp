// Copyright Codevedas Inc. 2025-present
//
// This source code is licensed under the MIT license found in the
// LICENSE file in the root directory of this source tree.

#include "request_processor.hpp"

#include <string>
#include <unistd.h>

namespace
{
  bool ascii_case_equal(char left, char right)
  {
    if (left >= 'A' && left <= 'Z')
    {
      left = static_cast<char>(left - 'A' + 'a');
    }

    if (right >= 'A' && right <= 'Z')
    {
      right = static_cast<char>(right - 'A' + 'a');
    }

    return left == right;
  }

  bool ascii_case_insensitive_equal(const std::string &left, const std::string &right)
  {
    if (left.size() != right.size())
    {
      return false;
    }

    for (std::size_t index = 0; index < left.size(); ++index)
    {
      if (!ascii_case_equal(left[index], right[index]))
      {
        return false;
      }
    }

    return true;
  }

  std::string strip_http_whitespace(const std::string &value)
  {
    const std::size_t start = value.find_first_not_of(" \t");
    if (start == std::string::npos)
    {
      return "";
    }

    const std::size_t end = value.find_last_not_of(" \t");
    return value.substr(start, end - start + 1);
  }

  bool header_named(const Vajra::request::ParsedHeader &header, const std::string &expected_name)
  {
    return ascii_case_insensitive_equal(header.name, expected_name);
  }

  bool header_value_contains_token(const std::string &value, const std::string &expected_token)
  {
    std::size_t cursor = 0;
    while (cursor <= value.size())
    {
      const std::size_t delimiter = value.find(',', cursor);
      const std::string token = strip_http_whitespace(value.substr(cursor, delimiter - cursor));
      if (!token.empty() && ascii_case_insensitive_equal(token, expected_token))
      {
        return true;
      }

      if (delimiter == std::string::npos)
      {
        break;
      }

      cursor = delimiter + 1;
    }

    return false;
  }

  bool content_length_is_zero(const std::string &value)
  {
    const std::string normalized = strip_http_whitespace(value);
    if (normalized.empty())
    {
      return false;
    }

    for (const char character : normalized)
    {
      if (character < '0' || character > '9')
      {
        return false;
      }
    }

    return normalized == "0";
  }

  class ClientSocketGuard
  {
  public:
    explicit ClientSocketGuard(int fd) : fd_(fd) {}
    ClientSocketGuard(const ClientSocketGuard &) = delete;
    ClientSocketGuard &operator=(const ClientSocketGuard &) = delete;
    ClientSocketGuard(ClientSocketGuard &&) = delete;
    ClientSocketGuard &operator=(ClientSocketGuard &&) = delete;

    ~ClientSocketGuard()
    {
      if (fd_ >= 0)
      {
        close(fd_);
      }
    }

  private:
    int fd_;
  };
}

Vajra::request::RequestProcessor::RequestProcessor(std::size_t max_request_head_bytes)
    : request_head_reader_(max_request_head_bytes),
      request_head_parser_(),
      response_writer_()
{
}

void Vajra::request::RequestProcessor::handle(int client_fd) const
{
  ClientSocketGuard client_socket_guard(client_fd);

  while (true)
  {
    HeadReadResult read_result;
    try
    {
      read_result = request_head_reader_.read(client_fd);
    }
    catch (const HeadError &error)
    {
      reject_request_head(client_fd, error);
      return;
    }

    if (!read_result.complete)
    {
      return;
    }

    ParsedRequest request;
    try
    {
      request = request_head_parser_.parse(read_result.request_head);
    }
    catch (const HeadError &error)
    {
      reject_request_head(client_fd, error);
      return;
    }

    const Vajra::response::ConnectionBehavior connection_behavior = connection_behavior_for(request);
    if (!response_writer_.send(client_fd, response_writer_.success_response(connection_behavior)))
    {
      return;
    }

    if (connection_behavior == Vajra::response::ConnectionBehavior::close)
    {
      return;
    }
  }
}

void Vajra::request::RequestProcessor::reject_request_head(int client_fd, const HeadError &error) const
{
  response_writer_.log_request_head_error(error);
  (void)response_writer_.send(client_fd, response_writer_.request_head_failure_response(error.kind()));
}

Vajra::response::ConnectionBehavior Vajra::request::RequestProcessor::connection_behavior_for(
    const ParsedRequest &request) const
{
  bool saw_content_length = false;

  for (const ParsedHeader &header : request.headers)
  {
    if (header_named(header, "Connection"))
    {
      if (header_value_contains_token(header.value, "close") || header_value_contains_token(header.value, "upgrade"))
      {
        return Vajra::response::ConnectionBehavior::close;
      }
    }

    if (header_named(header, "Upgrade") || header_named(header, "Transfer-Encoding"))
    {
      return Vajra::response::ConnectionBehavior::close;
    }

    if (header_named(header, "Content-Length"))
    {
      if (saw_content_length || !content_length_is_zero(header.value))
      {
        return Vajra::response::ConnectionBehavior::close;
      }

      saw_content_length = true;
    }
  }

  return Vajra::response::ConnectionBehavior::keep_alive;
}
