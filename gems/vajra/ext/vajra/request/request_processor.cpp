// Copyright Codevedas Inc. 2025-present
//
// This source code is licensed under the MIT license found in the
// LICENSE file in the root directory of this source tree.

#include "request_processor.hpp"

#include "http_field_utils.hpp"
#include "request_context.hpp"
#include "response/http_header_utils.hpp"
#include "response/response_serializer.hpp"
#include "runtime/runtime_logging.hpp"
#include "runtime/runtime_state.hpp"

#include <chrono>
#include <iostream>
#include <sstream>
#include <string>
#include <unistd.h>
#include <utility>
#include <vector>

namespace
{
  constexpr const char *kInternalTraceIdHeader = "X-Vajra-Internal-Trace-Id";
  constexpr const char *kInternalSpanIdHeader = "X-Vajra-Internal-Span-Id";

  void log_request_error(const std::string &message)
  {
    Vajra::runtime::log_runtime_error(message);
  }

  bool header_named(const Vajra::request::ParsedHeader &header, const std::string &expected_name)
  {
    return Vajra::request::ascii_case_insensitive_equal(header.name, expected_name);
  }

  std::string header_value(const Vajra::request::ParsedRequest &request, const std::string &name)
  {
    for (const auto &header : request.headers)
    {
      if (header_named(header, name))
      {
        return header.value;
      }
    }
    return "";
  }

  std::string traceparent_part(const std::string &traceparent, int part)
  {
    std::size_t cursor = 0;
    for (int index = 0; index < part; ++index)
    {
      cursor = traceparent.find('-', cursor);
      if (cursor == std::string::npos)
      {
        return "";
      }
      ++cursor;
    }
    const std::size_t end = traceparent.find('-', cursor);
    return traceparent.substr(cursor, end == std::string::npos ? std::string::npos : end - cursor);
  }

  bool internal_trace_header(const Vajra::response::Header &header)
  {
    return Vajra::response::lowercase_header_name(header.name) == "x-vajra-internal-trace-id" ||
           Vajra::response::lowercase_header_name(header.name) == "x-vajra-internal-span-id";
  }

  std::string response_header_value(const Vajra::response::Response &response, const std::string &name)
  {
    for (const auto &header : response.headers)
    {
      if (Vajra::request::ascii_case_insensitive_equal(header.name, name))
      {
        return header.value;
      }
    }
    return "";
  }

  void strip_internal_trace_headers(Vajra::response::Response &response)
  {
    std::vector<Vajra::response::Header> filtered_headers;
    filtered_headers.reserve(response.headers.size());
    for (const auto &header : response.headers)
    {
      if (!internal_trace_header(header))
      {
        filtered_headers.push_back(header);
      }
    }
    response.headers = std::move(filtered_headers);
  }

  Vajra::runtime::AccessLogEvent access_event_for(
      const Vajra::request::RequestContext &request_context,
      int status_code,
      std::size_t response_body_bytes,
      std::chrono::steady_clock::time_point started_at,
      const std::string &connection_outcome,
      const std::string &trace_id = "",
      const std::string &span_id = "")
  {
    const std::string traceparent = header_value(request_context.request, "traceparent");
    const std::size_t worker_index = Vajra::runtime::current_worker_index();
    return Vajra::runtime::AccessLogEvent{
        request_context.request.request_line.method,
        request_context.request.request_line.target,
        status_code,
        std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - started_at).count(),
        response_body_bytes,
        request_context.socket.remote_address,
        request_context.request.request_line.version,
        header_value(request_context.request, "host"),
        header_value(request_context.request, "user-agent"),
        header_value(request_context.request, "referer"),
        header_value(request_context.request, "x-request-id"),
        getpid(),
        static_cast<int>(worker_index),
        connection_outcome,
        trace_id.empty() ? traceparent_part(traceparent, 1) : trace_id,
        span_id.empty() ? traceparent_part(traceparent, 2) : span_id};
  }

  Vajra::runtime::AccessLogEvent access_event_for_unparsed_head(
      const Vajra::request::SocketContext &socket_context,
      int status_code,
      std::size_t response_body_bytes,
      std::chrono::steady_clock::time_point started_at)
  {
    return Vajra::runtime::AccessLogEvent{
        "",
        "",
        status_code,
        std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - started_at).count(),
        response_body_bytes,
        socket_context.remote_address,
        "",
        "",
        "",
        "",
        "",
        getpid(),
        static_cast<int>(Vajra::runtime::current_worker_index()),
        "close",
        "",
        ""};
  }

  void emit_native_request_observability(
      const Vajra::runtime::AccessLogEvent &event,
      const std::string &outcome,
      const std::string &failure_kind,
      bool response_sent,
      const std::string &error_message)
  {
    Vajra::runtime::emit_runtime_request_observability_event(
        event,
        outcome,
        failure_kind,
        response_sent,
        error_message);
  }

  std::string connection_outcome_for(Vajra::response::ConnectionBehavior behavior)
  {
    return behavior == Vajra::response::ConnectionBehavior::close ? "close" : "keepalive";
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

  class RequestWallClockProbe
  {
  public:
    RequestWallClockProbe() : started_at_(std::chrono::steady_clock::now()) {}

    ~RequestWallClockProbe()
    {
      Vajra::runtime::note_worker_request_time(
          std::chrono::duration_cast<std::chrono::nanoseconds>(
              std::chrono::steady_clock::now() - started_at_)
              .count());
    }

  private:
    std::chrono::steady_clock::time_point started_at_;
  };
}

Vajra::request::RequestProcessor::RequestProcessor(
    std::size_t max_request_head_bytes,
    std::shared_ptr<const RequestExecutor> request_executor)
    : RequestProcessor(max_request_head_bytes, 5, 30, 30, std::move(request_executor))
{
}

Vajra::request::RequestProcessor::RequestProcessor(
    std::size_t max_request_head_bytes,
    int request_head_timeout_seconds,
    int first_data_timeout_seconds,
    int persistent_timeout_seconds,
    std::shared_ptr<const RequestExecutor> request_executor)
    : request_head_reader_(max_request_head_bytes, request_head_timeout_seconds),
      request_head_parser_(),
      response_writer_(),
      request_executor_(std::move(request_executor)),
      first_data_timeout_seconds_(first_data_timeout_seconds),
      persistent_timeout_seconds_(persistent_timeout_seconds)
{
}

void Vajra::request::RequestProcessor::handle(int client_fd, const SocketContext &socket_context) const
{
  ClientSocketGuard client_socket_guard(client_fd);
  std::string buffered_bytes;
  bool first_request = true;

  while (true)
  {
    RequestProcessingResult result = handle_one(
        client_fd,
        socket_context,
        std::move(buffered_bytes),
        first_request);
    if (result.outcome != RequestProcessingOutcome::keep_alive)
    {
      return;
    }

    buffered_bytes = std::move(result.buffered_bytes);
    first_request = result.first_request;
  }
}

Vajra::request::RequestProcessingResult Vajra::request::RequestProcessor::handle_one(
    int client_fd,
    const SocketContext &socket_context,
    std::string buffered_bytes,
    bool first_request) const
{
  HeadReadResult read_result;
  try
  {
    const auto head_started_at = std::chrono::steady_clock::now();
    read_result = request_head_reader_.read(
        client_fd,
        std::move(buffered_bytes),
        first_request ? first_data_timeout_seconds_ : persistent_timeout_seconds_);
    Vajra::runtime::note_worker_request_head_time(
        std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::now() - head_started_at)
            .count());
  }
  catch (const HeadError &error)
  {
    const auto response = response_writer_.request_head_failure_response(error.kind());
    reject_request_head(client_fd, error);
    emit_native_request_observability(
        access_event_for_unparsed_head(socket_context, response.status.code, response.body.size(), std::chrono::steady_clock::now()),
        "request_head_error",
        error.what(),
        true,
        error.what());
    return RequestProcessingResult{RequestProcessingOutcome::close, "", first_request};
  }

  if (!read_result.complete)
  {
    return RequestProcessingResult{
        read_result.peer_closed ? RequestProcessingOutcome::close : RequestProcessingOutcome::await_read,
        std::move(read_result.request_head),
        first_request};
  }

  RequestWallClockProbe request_wall_clock_probe;
  const auto request_started_at = std::chrono::steady_clock::now();
  buffered_bytes = std::move(read_result.trailing_bytes);

  RequestContext request_context;
  try
  {
    const auto parse_started_at = std::chrono::steady_clock::now();
    request_context = RequestContext{
        request_head_parser_.parse(read_result.request_head),
        socket_context,
        client_fd,
        ""};
    Vajra::runtime::note_worker_request_parse_time(
        std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::now() - parse_started_at)
            .count());
  }
  catch (const HeadError &error)
  {
    const auto response = response_writer_.request_head_failure_response(error.kind());
    reject_request_head(client_fd, error);
    emit_native_request_observability(
        access_event_for_unparsed_head(socket_context, response.status.code, response.body.size(), request_started_at),
        "request_parse_error",
        error.what(),
        true,
        error.what());
    return RequestProcessingResult{RequestProcessingOutcome::close, "", first_request};
  }

  const Vajra::response::ConnectionBehavior connection_behavior = connection_behavior_for(request_context.request);
  if (request_executor_)
  {
    std::optional<Vajra::response::Response> control_response = request_executor_->control_response(request_context);
    if (control_response)
    {
      control_response->headers = Vajra::response::strip_framing_headers(control_response->headers);
      control_response->connection_behavior = connection_behavior;
      if (!response_writer_.send(client_fd, *control_response))
      {
        return RequestProcessingResult{RequestProcessingOutcome::close, "", false};
      }
      Vajra::runtime::note_worker_request_completed();
      Vajra::runtime::log_access_event(access_event_for(
          request_context,
          control_response->status.code,
          control_response->body.size(),
          request_started_at,
          connection_outcome_for(connection_behavior)));
      return RequestProcessingResult{
          connection_behavior == Vajra::response::ConnectionBehavior::close
              ? RequestProcessingOutcome::close
              : RequestProcessingOutcome::keep_alive,
          std::move(buffered_bytes),
          false};
    }
  }

  Vajra::response::Response response;
  try
  {
    std::unique_ptr<RequestExecutionSession> execution_session;
    if (request_executor_)
    {
      const auto rack_start_started_at = std::chrono::steady_clock::now();
      execution_session = request_executor_->start(request_context);
      Vajra::runtime::note_worker_rack_start_time(
          std::chrono::duration_cast<std::chrono::nanoseconds>(
              std::chrono::steady_clock::now() - rack_start_started_at)
              .count());
    }
    else
    {
      execution_session = nullptr;
    }

    const auto body_started_at = std::chrono::steady_clock::now();
    BodyReadResult body_read_result = request_body_reader_.stream_read(
        client_fd,
        request_context.request,
        [&request_context, &execution_session](const std::string &chunk) {
          if (execution_session)
          {
            execution_session->append_request_body_chunk(chunk);
            return;
          }

          request_context.request_body.append(chunk);
        },
        std::move(buffered_bytes));
    Vajra::runtime::note_worker_request_body_time(
        std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::now() - body_started_at)
            .count());
    buffered_bytes = std::move(body_read_result.remaining_buffered_bytes);

    if (execution_session)
    {
      const auto rack_finish_started_at = std::chrono::steady_clock::now();
      response = response_for(*execution_session, connection_behavior);
      Vajra::runtime::note_worker_rack_finish_time(
          std::chrono::duration_cast<std::chrono::nanoseconds>(
              std::chrono::steady_clock::now() - rack_finish_started_at)
              .count());
    }
    else
    {
      const auto rack_finish_started_at = std::chrono::steady_clock::now();
      response = response_for(request_context, connection_behavior);
      Vajra::runtime::note_worker_rack_finish_time(
          std::chrono::duration_cast<std::chrono::nanoseconds>(
              std::chrono::steady_clock::now() - rack_finish_started_at)
              .count());
    }

    Vajra::response::ResponseSerializer().validate(response);
  }
  catch (const BodyReadIncompleteError &)
  {
    emit_native_request_observability(
        access_event_for(request_context, 0, 0, request_started_at, "close"),
        "request_body_error",
        "request_body_incomplete",
        false,
        "request body read incomplete");
    return RequestProcessingResult{RequestProcessingOutcome::close, "", false};
  }
  catch (const HeadError &error)
  {
    const auto rejection_response = response_writer_.request_head_failure_response(error.kind());
    reject_request_head(client_fd, error);
    const auto event = access_event_for(
        request_context,
        rejection_response.status.code,
        rejection_response.body.size(),
        request_started_at,
        "close");
    emit_native_request_observability(event, "request_head_error", error.what(), true, error.what());
    Vajra::runtime::log_access_event(access_event_for(
        request_context,
        rejection_response.status.code,
        rejection_response.body.size(),
        request_started_at,
        "close"));
    return RequestProcessingResult{RequestProcessingOutcome::close, "", false};
  }
  catch (const QueueCapacityError &error)
  {
    const auto rejection_response = response_writer_.queue_capacity_response();
    reject_request_queue_capacity(client_fd, error);
    const auto event = access_event_for(
        request_context,
        rejection_response.status.code,
        rejection_response.body.size(),
        request_started_at,
        "close");
    emit_native_request_observability(event, "queue_capacity", "queue_capacity", true, error.what());
    Vajra::runtime::log_access_event(event);
    return RequestProcessingResult{RequestProcessingOutcome::close, "", false};
  }
  catch (const RequestTimeoutError &error)
  {
    const auto rejection_response = response_writer_.request_timeout_response();
    reject_request_timeout(client_fd, error);
    const auto event = access_event_for(
        request_context,
        rejection_response.status.code,
        rejection_response.body.size(),
        request_started_at,
        "close");
    emit_native_request_observability(event, "request_timeout", "request_timeout", true, error.what());
    Vajra::runtime::log_access_event(event);
    return RequestProcessingResult{RequestProcessingOutcome::close, "", false};
  }
  catch (const std::exception &error)
  {
    const auto rejection_response = response_writer_.internal_server_error_response();
    reject_request_execution(client_fd, error);
    const auto event = access_event_for(
        request_context,
        rejection_response.status.code,
        rejection_response.body.size(),
        request_started_at,
        "close");
    emit_native_request_observability(event, "execution_error", "execution_error", true, error.what());
    Vajra::runtime::log_access_event(event);
    return RequestProcessingResult{RequestProcessingOutcome::close, "", false};
  }

  const std::string response_trace_id = response_header_value(response, kInternalTraceIdHeader);
  const std::string response_span_id = response_header_value(response, kInternalSpanIdHeader);
  strip_internal_trace_headers(response);
  if (!response_writer_.send(client_fd, response))
  {
    return RequestProcessingResult{RequestProcessingOutcome::close, "", false};
  }
  Vajra::runtime::note_worker_request_completed();
  Vajra::runtime::log_access_event(access_event_for(
      request_context,
      response.status.code,
      response.body.size(),
      request_started_at,
      connection_outcome_for(connection_behavior),
      response_trace_id,
      response_span_id));

  return RequestProcessingResult{
      connection_behavior == Vajra::response::ConnectionBehavior::close
          ? RequestProcessingOutcome::close
          : RequestProcessingOutcome::keep_alive,
      std::move(buffered_bytes),
      false};
}

Vajra::response::Response Vajra::request::RequestProcessor::response_for(
    RequestExecutionSession &execution_session,
    Vajra::response::ConnectionBehavior connection_behavior) const
{
  std::optional<Vajra::response::Response> response = execution_session.finish();
  if (!response)
  {
    return response_writer_.success_response(connection_behavior);
  }

  response->headers = Vajra::response::strip_framing_headers(response->headers);
  response->connection_behavior = connection_behavior;
  return *response;
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

void Vajra::request::RequestProcessor::reject_request_queue_capacity(
    int client_fd,
    const QueueCapacityError &error) const
{
  std::ostringstream message;
  message << "request queue capacity reached: client_fd=" << client_fd << " error=" << error.what();
  log_request_error(message.str());
  (void)response_writer_.send(client_fd, response_writer_.queue_capacity_response());
}

void Vajra::request::RequestProcessor::reject_request_timeout(int client_fd, const RequestTimeoutError &error) const
{
  std::ostringstream message;
  message << "request timed out: client_fd=" << client_fd << " error=" << error.what();
  log_request_error(message.str());
  (void)response_writer_.send(client_fd, response_writer_.request_timeout_response());
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
      if (saw_content_length)
      {
        return Vajra::response::ConnectionBehavior::close;
      }

      saw_content_length = true;
    }
  }

  return Vajra::response::ConnectionBehavior::keep_alive;
}
