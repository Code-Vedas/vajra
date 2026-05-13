// Copyright Codevedas Inc. 2025-present
//
// This source code is licensed under the MIT license found in the
// LICENSE file in the root directory of this source tree.

#include "request_processor.hpp"

#include "http_field_utils.hpp"
#include "request_context.hpp"
#include "response/http_header_utils.hpp"
#include "response/response_serializer.hpp"

#include <chrono>
#include <ctime>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <string>
#include <unistd.h>
#include <utility>
#include <vector>

namespace
{
  std::string utc_timestamp()
  {
    const auto now = std::chrono::system_clock::now();
    const std::time_t now_time = std::chrono::system_clock::to_time_t(now);
    std::tm utc_time{};
    gmtime_r(&now_time, &utc_time);

    std::ostringstream timestamp;
    timestamp << std::put_time(&utc_time, "%Y-%m-%dT%H:%M:%SZ");
    return timestamp.str();
  }

  void log_request_error(const std::string &message)
  {
    std::cerr << "[Vajra][error] " << utc_timestamp() << ' ' << message << std::endl;
  }

  bool header_named(const Vajra::request::ParsedHeader &header, const std::string &expected_name)
  {
    return Vajra::request::ascii_case_insensitive_equal(header.name, expected_name);
  }

  bool header_value_contains_token(const std::string &value, const std::string &expected_token)
  {
    std::size_t cursor = 0;
    while (cursor <= value.size())
    {
      const std::size_t delimiter = value.find(',', cursor);
      const std::string token = Vajra::request::strip_http_whitespace(value.substr(cursor, delimiter - cursor));
      if (!token.empty() && Vajra::request::ascii_case_insensitive_equal(token, expected_token))
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

Vajra::request::RequestProcessor::RequestProcessor(
    std::size_t max_request_head_bytes,
    std::shared_ptr<const RequestExecutor> request_executor)
    : request_head_reader_(max_request_head_bytes),
      request_head_parser_(),
      response_writer_(),
      request_executor_(std::move(request_executor))
{
}

void Vajra::request::RequestProcessor::handle(int client_fd, const SocketContext &socket_context) const
{
  ClientSocketGuard client_socket_guard(client_fd);
  std::string buffered_bytes;

  while (true)
  {
    HeadReadResult read_result;
    try
    {
      read_result = request_head_reader_.read(client_fd, std::move(buffered_bytes));
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

    buffered_bytes = std::move(read_result.trailing_bytes);

    RequestContext request_context;
    try
    {
      request_context = RequestContext{
          request_head_parser_.parse(read_result.request_head),
          socket_context,
          ""};
    }
    catch (const HeadError &error)
    {
      reject_request_head(client_fd, error);
      return;
    }

    try
    {
      BodyReadResult body_read_result =
          request_body_reader_.read(client_fd, request_context.request, std::move(buffered_bytes));
      request_context.request_body = std::move(body_read_result.body);
      buffered_bytes = std::move(body_read_result.remaining_buffered_bytes);
    }
    catch (const BodyReadIncompleteError &)
    {
      return;
    }
    catch (const HeadError &error)
    {
      reject_request_head(client_fd, error);
      return;
    }

    const Vajra::response::ConnectionBehavior connection_behavior = connection_behavior_for(request_context.request);

    Vajra::response::Response response;
    try
    {
      response = response_for(request_context, connection_behavior);
    }
    catch (const HeadError &error)
    {
      reject_request_head(client_fd, error);
      return;
    }
    catch (const std::exception &error)
    {
      reject_request_execution(client_fd, error);
      return;
    }

    if (!response_writer_.send(client_fd, response))
    {
      return;
    }

    if (connection_behavior == Vajra::response::ConnectionBehavior::close)
    {
      return;
    }
  }
}

Vajra::response::Response Vajra::request::RequestProcessor::response_for(
    const RequestContext &request_context,
    Vajra::response::ConnectionBehavior connection_behavior) const
{
  if (!request_executor_)
  {
    return response_writer_.success_response(connection_behavior);
  }

  std::optional<Vajra::response::Response> response = request_executor_->execute(request_context);
  if (!response)
  {
    return response_writer_.success_response(connection_behavior);
  }

  response->headers = Vajra::response::strip_framing_headers(response->headers);
  response->connection_behavior = connection_behavior;
  Vajra::response::ResponseSerializer serializer;
  serializer.validate(*response);
  return *response;
}

void Vajra::request::RequestProcessor::reject_request_head(int client_fd, const HeadError &error) const
{
  response_writer_.log_request_head_error(error);
  (void)response_writer_.send(client_fd, response_writer_.request_head_failure_response(error.kind()));
}

void Vajra::request::RequestProcessor::reject_request_execution(int client_fd, const std::exception &error) const
{
  std::ostringstream message;
  message << "request execution failed: client_fd=" << client_fd << " error=" << error.what();
  log_request_error(message.str());
  (void)response_writer_.send(client_fd, response_writer_.internal_server_error_response());
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
      if (saw_content_length || !Vajra::request::content_length_is_zero(header.value))
      {
        return Vajra::response::ConnectionBehavior::close;
      }

      saw_content_length = true;
    }
  }

  return Vajra::response::ConnectionBehavior::keep_alive;
}
