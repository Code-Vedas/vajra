// Copyright Codevedas Inc. 2025-present
//
// This source code is licensed under the MIT license found in the
// LICENSE file in the root directory of this source tree.

#include "request_processor.hpp"

#include "http_field_utils.hpp"
#include "response/http_header_utils.hpp"
#include "request_head_error.hpp"
#include "runtime/runtime_logging.hpp"
#include "runtime/runtime_state.hpp"
#include "runtime/traceparent.hpp"
#include "rack/native_input.hpp"
#include "transport/tls_connection.hpp"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <optional>
#include <sstream>
#include <string>
#include <unistd.h>
#include <utility>
#include <vector>

namespace
{
    constexpr const char *kInternalTraceIdHeader = "X-Vajra-Internal-Trace-Id";
    constexpr const char *kInternalSpanIdHeader = "X-Vajra-Internal-Span-Id";

    std::uint64_t elapsed_nanoseconds(std::chrono::steady_clock::time_point started_at)
    {
        return std::chrono::duration_cast<std::chrono::nanoseconds>(
                   std::chrono::steady_clock::now() - started_at)
            .count();
    }

    bool header_named(const Vajra::request::ParsedHeader &header, const std::string &expected_name)
    {
        return Vajra::request::ascii_case_insensitive_equal(header.name, expected_name);
    }

    const char *head_failure_kind_token(Vajra::request::HeadFailureKind kind)
    {
        switch (kind)
        {
        case Vajra::request::HeadFailureKind::bad_request:
            return "bad_request";
        case Vajra::request::HeadFailureKind::header_too_large:
            return "header_too_large";
        }

        return "bad_request";
    }

    struct ObservedRequestHeaders
    {
        std::string host;
        std::string user_agent;
        std::string referer;
        std::string request_id;
        std::string traceparent;
    };

    bool request_headers_needed(const Vajra::runtime::AccessLogFieldNeeds &needs)
    {
        return needs.host ||
               needs.user_agent ||
               needs.referer ||
               needs.request_id ||
               needs.trace_context;
    }

    ObservedRequestHeaders observed_request_headers(
        const Vajra::request::ParsedRequest &request,
        const Vajra::runtime::AccessLogFieldNeeds &needs)
    {
        ObservedRequestHeaders observed;
        for (const auto &header : request.headers)
        {
            if (needs.host && header_named(header, "host"))
            {
                observed.host = header.value;
            }
            else if (needs.user_agent && header_named(header, "user-agent"))
            {
                observed.user_agent = header.value;
            }
            else if (needs.referer && header_named(header, "referer"))
            {
                observed.referer = header.value;
            }
            else if (needs.request_id && header_named(header, "x-request-id"))
            {
                observed.request_id = header.value;
            }
            else if (needs.trace_context && header_named(header, "traceparent"))
            {
                observed.traceparent = header.value;
            }
        }
        return observed;
    }

    bool internal_trace_header(const Vajra::response::Header &header)
    {
        return Vajra::request::ascii_case_insensitive_equal(header.name, "x-vajra-internal-trace-id") ||
               Vajra::request::ascii_case_insensitive_equal(header.name, "x-vajra-internal-span-id");
    }

    struct InternalTraceResponseHeaders
    {
        std::string trace_id;
        std::string span_id;
        bool present = false;
    };

    InternalTraceResponseHeaders internal_trace_response_headers(const Vajra::response::Response &response)
    {
        InternalTraceResponseHeaders headers;
        for (const auto &header : response.headers)
        {
            if (Vajra::request::ascii_case_insensitive_equal(header.name, kInternalTraceIdHeader))
            {
                headers.trace_id = header.value;
                headers.present = true;
            }
            else if (Vajra::request::ascii_case_insensitive_equal(header.name, kInternalSpanIdHeader))
            {
                headers.span_id = header.value;
                headers.present = true;
            }
        }
        return headers;
    }

    void strip_internal_trace_headers(Vajra::response::Response &response)
    {
        if (response.headers.empty())
        {
            return;
        }

        const auto first_internal_header = std::find_if(
            response.headers.begin(),
            response.headers.end(),
            [](const Vajra::response::Header &header)
            {
                return internal_trace_header(header);
            });
        if (first_internal_header == response.headers.end())
        {
            return;
        }

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
        const Vajra::runtime::AccessLogFieldNeeds needs = Vajra::runtime::access_log_field_needs();
        const ObservedRequestHeaders headers = request_headers_needed(needs)
                                                   ? observed_request_headers(request_context.request, needs)
                                                   : ObservedRequestHeaders{};
        const bool use_incoming_trace_context =
            needs.trace_context && !headers.traceparent.empty() && trace_id.empty() && span_id.empty();
        return Vajra::runtime::AccessLogEvent{
            request_context.request.request_line.method,
            request_context.request.request_line.target,
            status_code,
            static_cast<std::int64_t>(elapsed_nanoseconds(started_at)),
            response_body_bytes,
            request_context.socket.remote_address,
            request_context.request.request_line.version,
            headers.host,
            headers.user_agent,
            headers.referer,
            headers.request_id,
            getpid(),
            static_cast<int>(Vajra::runtime::current_worker_index()),
            connection_outcome,
            use_incoming_trace_context ? Vajra::runtime::traceparent_part(headers.traceparent, 1) : trace_id,
            use_incoming_trace_context ? Vajra::runtime::traceparent_part(headers.traceparent, 2) : span_id};
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
            static_cast<std::int64_t>(elapsed_nanoseconds(started_at)),
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

    void log_access_event_if_enabled(const Vajra::runtime::AccessLogEvent &event)
    {
        if (Vajra::runtime::access_logging_enabled())
        {
            Vajra::runtime::log_access_event(event);
        }
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

    struct H2cUpgradeValidation
    {
        bool attempted = false;
        bool valid = false;
        std::vector<std::uint8_t> settings_payload;
    };

    int base64url_value(char character)
    {
        if (character >= 'A' && character <= 'Z')
        {
            return character - 'A';
        }
        if (character >= 'a' && character <= 'z')
        {
            return character - 'a' + 26;
        }
        if (character >= '0' && character <= '9')
        {
            return character - '0' + 52;
        }
        if (character == '-')
        {
            return 62;
        }
        if (character == '_')
        {
            return 63;
        }
        return -1;
    }

    std::optional<std::vector<std::uint8_t>> decode_base64url(const std::string &encoded_value)
    {
        const std::string value = Vajra::request::strip_http_whitespace(encoded_value);
        if (value.size() % 4 == 1)
        {
            return std::nullopt;
        }

        std::vector<std::uint8_t> decoded;
        decoded.reserve((value.size() * 3) / 4);
        std::uint32_t accumulator = 0;
        int bits = 0;
        bool padding_started = false;

        for (const char character : value)
        {
            if (character == '=')
            {
                padding_started = true;
                continue;
            }
            if (padding_started)
            {
                return std::nullopt;
            }

            const int digit = base64url_value(character);
            if (digit < 0)
            {
                return std::nullopt;
            }
            accumulator = (accumulator << 6) | static_cast<std::uint32_t>(digit);
            bits += 6;
            while (bits >= 8)
            {
                bits -= 8;
                decoded.push_back(static_cast<std::uint8_t>((accumulator >> bits) & 0xffu));
            }
        }

        if (bits > 0)
        {
            const std::uint32_t mask = (1u << bits) - 1u;
            if ((accumulator & mask) != 0)
            {
                return std::nullopt;
            }
        }

        return decoded;
    }

    H2cUpgradeValidation validate_h2c_upgrade(const Vajra::request::ParsedRequest &request)
    {
        H2cUpgradeValidation validation;
        bool upgrade_h2c = false;
        bool connection_upgrade = false;
        bool connection_http2_settings = false;
        bool has_transfer_encoding = false;
        bool has_nonzero_body = false;
        std::size_t settings_header_count = 0;
        std::string settings_header_value;

        for (const auto &header : request.headers)
        {
            if (header_named(header, "Upgrade") && header_value_contains_token(header.value, "h2c"))
            {
                upgrade_h2c = true;
            }
            else if (header_named(header, "Connection"))
            {
                connection_upgrade = connection_upgrade || header_value_contains_token(header.value, "upgrade");
                connection_http2_settings = connection_http2_settings || header_value_contains_token(header.value, "http2-settings");
            }
            else if (header_named(header, "HTTP2-Settings"))
            {
                ++settings_header_count;
                settings_header_value = header.value;
            }
            else if (header_named(header, "Transfer-Encoding"))
            {
                has_transfer_encoding = true;
            }
            else if (header_named(header, "Content-Length") && !Vajra::request::content_length_is_zero(header.value))
            {
                has_nonzero_body = true;
            }
        }

        validation.attempted = upgrade_h2c || connection_http2_settings || settings_header_count > 0;
        if (!validation.attempted)
        {
            return validation;
        }

        const auto settings_payload = settings_header_count == 1 ? decode_base64url(settings_header_value) : std::nullopt;
        validation.valid =
            request.request_line.version == "HTTP/1.1" &&
            upgrade_h2c &&
            connection_upgrade &&
            connection_http2_settings &&
            settings_header_count == 1 &&
            settings_payload.has_value() &&
            !has_transfer_encoding &&
            !has_nonzero_body;
        if (validation.valid)
        {
            validation.settings_payload = std::move(*settings_payload);
        }
        return validation;
    }

    bool write_all(Vajra::transport::Connection &connection, const char *data, std::size_t length)
    {
        std::size_t written = 0;
        while (written < length)
        {
            const ssize_t result = connection.write(data + written, length - written);
            if (result <= 0)
            {
                return false;
            }
            written += static_cast<std::size_t>(result);
        }
        return true;
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

    class RequestWallClockRecorder
    {
    public:
        RequestWallClockRecorder() : started_at_(std::chrono::steady_clock::now()) {}

        ~RequestWallClockRecorder()
        {
            Vajra::runtime::note_worker_request_time(elapsed_nanoseconds(started_at_));
        }

    private:
        std::chrono::steady_clock::time_point started_at_;
    };
}

namespace Vajra
{
    namespace rack
    {
        std::shared_ptr<NativeHijackTransport> tls_native_hijack_transport(
            Vajra::transport::TlsConnection &connection);
    }
}

Vajra::request::RequestProcessor::RequestProcessor(
    std::size_t max_request_head_bytes,
    std::shared_ptr<const RequestExecutor> request_executor)
    : RequestProcessor(max_request_head_bytes, kDefaultMaxRequestBodyBytes, 5, 30, kDefaultRequestBodyTimeoutSeconds, 30, 0, std::move(request_executor))
{
}

Vajra::request::RequestProcessor::RequestProcessor(
    std::size_t max_request_head_bytes,
    std::size_t max_request_body_bytes,
    int request_head_timeout_seconds,
    int first_data_timeout_seconds,
    int request_body_timeout_seconds,
    int persistent_timeout_seconds,
    std::size_t max_keepalive_requests,
    std::shared_ptr<const RequestExecutor> request_executor,
    std::size_t http2_execution_threads,
    bool http2_enabled,
    Http2Config http2_config)
    : request_head_reader_(max_request_head_bytes, request_head_timeout_seconds),
      request_head_parser_(),
      request_body_reader_(max_request_body_bytes, kDefaultMaxChunkLineBytes, kDefaultMaxTrailerLineBytes, request_body_timeout_seconds),
      response_writer_(),
      request_executor_(std::move(request_executor)),
      http2_execution_pool_(std::make_shared<Http2ExecutionPool>(std::max<std::size_t>(1, http2_execution_threads))),
      http2_enabled_(http2_enabled),
      http2_config_(http2_config),
      first_data_timeout_seconds_(first_data_timeout_seconds),
      persistent_timeout_seconds_(persistent_timeout_seconds),
      max_keepalive_requests_(max_keepalive_requests)
{
    if (http2_config_.max_request_head_bytes == 0)
    {
        http2_config_.max_request_head_bytes = max_request_head_bytes;
    }
    http2_config_.max_request_body_bytes = max_request_body_bytes;
    http2_config_.max_keepalive_requests = max_keepalive_requests;
}

Vajra::request::RequestProcessingOutcome Vajra::request::RequestProcessor::handle(
    Vajra::transport::Connection &connection,
    const SocketContext &socket_context) const
{
    std::string buffered_bytes;
    bool first_request = true;
    std::size_t completed_requests = 0;
    for (;;)
    {
        const bool force_close = max_keepalive_requests_ > 0 && completed_requests + 1 >= max_keepalive_requests_;
        const RequestProcessingResult result = handle_one(
            connection,
            socket_context,
            std::move(buffered_bytes),
            first_request,
            force_close);

        if (result.outcome != RequestProcessingOutcome::keep_alive)
        {
            return result.outcome;
        }
        ++completed_requests;
        buffered_bytes = std::move(result.buffered_bytes);
        first_request = false;
    }
    return RequestProcessingOutcome::close;
}

std::shared_ptr<Vajra::request::Http2ExecutionPool> Vajra::request::RequestProcessor::http2_execution_pool() const
{
    return http2_execution_pool_;
}

Vajra::request::RequestProcessingResult Vajra::request::RequestProcessor::handle_one(
    Vajra::transport::Connection &connection,
    const SocketContext &socket_context,
    std::string buffered_bytes,
    bool first_request,
    bool force_close_after_response) const
{
    const auto started_at = std::chrono::steady_clock::now();
    HeadReadResult head_read_result;
    try
    {
        head_read_result = request_head_reader_.read(
            connection,
            std::move(buffered_bytes),
            first_request ? first_data_timeout_seconds_ : persistent_timeout_seconds_);
    }
    catch (const HeadError &error)
    {
        Vajra::runtime::note_worker_request_head_time(elapsed_nanoseconds(started_at));
        const auto response = response_writer_.request_head_failure_response(error.kind());
        const bool response_sent = reject_request_head(connection, error, response);
        emit_native_request_observability(
            access_event_for_unparsed_head(
                socket_context,
                response.status.code,
                Vajra::response::response_body_size(response),
                started_at),
            "request_head_error",
            head_failure_kind_token(error.kind()),
            response_sent,
            error.what());
        return RequestProcessingResult{RequestProcessingOutcome::close, "", false};
    }
    catch (const RequestTimeoutError &error)
    {
        Vajra::runtime::note_worker_request_head_time(elapsed_nanoseconds(started_at));
        const auto response = response_writer_.request_timeout_response();
        const bool response_sent = reject_request_timeout(connection, error, response);
        emit_native_request_observability(
            access_event_for_unparsed_head(
                socket_context,
                response.status.code,
                Vajra::response::response_body_size(response),
                started_at),
            "request_timeout",
            "request_timeout",
            response_sent,
            error.what());
        return RequestProcessingResult{RequestProcessingOutcome::close, "", false};
    }

    Vajra::runtime::note_worker_request_head_time(elapsed_nanoseconds(started_at));

    if (!head_read_result.complete)
    {
        return RequestProcessingResult{
            head_read_result.peer_closed ? RequestProcessingOutcome::close : RequestProcessingOutcome::await_read,
            std::move(head_read_result.request_head),
            first_request};
    }

    RequestWallClockRecorder request_wall_clock_recorder;
    const auto request_started_at = std::chrono::steady_clock::now();

    RequestContext request_context;
    try
    {
        const auto parse_started_at = std::chrono::steady_clock::now();
        request_context.request = request_head_parser_.parse(head_read_result.request_head);
        Vajra::runtime::note_worker_request_parse_time(elapsed_nanoseconds(parse_started_at));
    }
    catch (const HeadError &error)
    {
        const auto response = response_writer_.request_head_failure_response(error.kind());
        const bool response_sent = reject_request_head(connection, error, response);
        emit_native_request_observability(
            access_event_for_unparsed_head(
                socket_context,
                response.status.code,
                Vajra::response::response_body_size(response),
                request_started_at),
            "request_parse_error",
            head_failure_kind_token(error.kind()),
            response_sent,
            error.what());
        return RequestProcessingResult{RequestProcessingOutcome::close, "", false};
    }

    request_context.client_fd = connection.fd();
    request_context.socket = socket_context;
    if (auto *tls_connection = dynamic_cast<Vajra::transport::TlsConnection *>(&connection))
    {
        request_context.native_hijack_transport = Vajra::rack::tls_native_hijack_transport(*tls_connection);
    }

    if (request_context.request.request_line.version == "HTTP/2.0" ||
        request_context.request.request_line.version == "HTTP/2")
    {
        return RequestProcessingResult{RequestProcessingOutcome::keep_alive, std::move(head_read_result.trailing_bytes), false};
    }

    H2cUpgradeValidation h2c_upgrade = validate_h2c_upgrade(request_context.request);
    if (h2c_upgrade.attempted)
    {
        if (!http2_enabled_ || !h2c_upgrade.valid)
        {
            const auto response = response_writer_.request_head_failure_response(HeadFailureKind::bad_request);
            const bool response_sent = reject_request_head(
                connection,
                bad_request_error("invalid h2c upgrade request"),
                response);
            emit_native_request_observability(
                access_event_for(
                    request_context,
                    response.status.code,
                    Vajra::response::response_body_size(response),
                    request_started_at,
                    "close"),
                "request_head_error",
                "bad_request",
                response_sent,
                "invalid h2c upgrade request");
            return RequestProcessingResult{RequestProcessingOutcome::close, "", false};
        }

        static constexpr const char kSwitchingProtocolsResponse[] =
            "HTTP/1.1 101 Switching Protocols\r\n"
            "Connection: Upgrade\r\n"
            "Upgrade: h2c\r\n"
            "\r\n";
        if (!write_all(connection, kSwitchingProtocolsResponse, sizeof(kSwitchingProtocolsResponse) - 1))
        {
            return RequestProcessingResult{RequestProcessingOutcome::close, "", false};
        }

        Http2UpgradeRequest upgrade_request{
            request_context,
            std::move(h2c_upgrade.settings_payload),
            std::move(head_read_result.trailing_bytes)};
        Http2Session session(
            connection,
            socket_context,
            http2_config_,
            request_executor_,
            http2_execution_pool_,
            std::move(upgrade_request));
        session.run();
        return RequestProcessingResult{RequestProcessingOutcome::close, "", false};
    }

    Vajra::response::ConnectionBehavior connection_behavior = connection_behavior_for(request_context.request);
    if (force_close_after_response)
    {
        connection_behavior = Vajra::response::ConnectionBehavior::close;
    }

    if (request_executor_)
    {
        std::optional<Vajra::response::Response> control_response = request_executor_->control_response(request_context);
        if (control_response)
        {
            control_response->headers = Vajra::response::strip_framing_headers(control_response->headers);
            strip_internal_trace_headers(*control_response);
            control_response->connection_behavior = connection_behavior;
            if (!response_writer_.send(connection, *control_response))
            {
                return RequestProcessingResult{RequestProcessingOutcome::close, "", false};
            }

            Vajra::runtime::note_worker_request_completed();
            const auto event = access_event_for(
                request_context,
                control_response->status.code,
                Vajra::response::response_body_size(*control_response),
                request_started_at,
                connection_outcome_for(connection_behavior));
            log_access_event_if_enabled(event);
            emit_native_request_observability(event, "ok", "", true, "");
            return RequestProcessingResult{
                connection_behavior == Vajra::response::ConnectionBehavior::keep_alive ? RequestProcessingOutcome::keep_alive : RequestProcessingOutcome::close,
                std::move(head_read_result.trailing_bytes),
                false};
        }
    }

    BodyReadPlan body_plan;
    bool execute_without_streaming = false;
    try
    {
        body_plan = request_body_reader_.plan_for(request_context.request);
        execute_without_streaming = body_plan.framing == BodyFraming::none ||
                                    request_body_reader_.can_read_without_streaming(
                                        body_plan,
                                        head_read_result.trailing_bytes);
    }
    catch (const HeadError &error)
    {
        const auto rejection_response = response_writer_.request_head_failure_response(error.kind());
        const bool response_sent = reject_request_head(connection, error, rejection_response);
        const auto event = access_event_for(
            request_context,
            rejection_response.status.code,
            Vajra::response::response_body_size(rejection_response),
            request_started_at,
            "close");
        emit_native_request_observability(
            event,
            "request_head_error",
            head_failure_kind_token(error.kind()),
            response_sent,
            error.what());
        log_access_event_if_enabled(event);
        return RequestProcessingResult{RequestProcessingOutcome::close, "", false};
    }

    std::unique_ptr<RequestExecutionSession> execution_session;
    std::shared_ptr<Vajra::rack::NativeInputState> native_input_owner;

    auto start_execution_session = [&]() -> std::shared_ptr<Vajra::rack::NativeInputState>
    {
        if (!request_executor_ || execute_without_streaming)
        {
            return nullptr;
        }
        if (!execution_session)
        {
            try
            {
                const auto rack_start_started_at = std::chrono::steady_clock::now();
                execution_session = request_executor_->start(request_context);
                Vajra::runtime::note_worker_rack_start_time(elapsed_nanoseconds(rack_start_started_at));
                if (execution_session)
                {
                    native_input_owner = execution_session->native_input_state_owner();
                }
            }
            catch (const QueueCapacityError &error)
            {
                reject_request_queue_capacity(connection, error, response_writer_.queue_capacity_response());
                throw;
            }
        }
        return native_input_owner;
    };

    if (request_executor_ && !execute_without_streaming)
    {
        native_input_owner = start_execution_session();
    }

    const auto body_started_at = std::chrono::steady_clock::now();
    BodyReadResult body_read_result;
    try
    {
        if (execute_without_streaming)
        {
            body_read_result = request_body_reader_.read(
                connection,
                request_context.request,
                std::move(head_read_result.trailing_bytes));
            request_context.request_body = std::move(body_read_result.body);
        }
        else
        {
            body_read_result = request_body_reader_.stream_read(
                connection,
                request_context.request,
                [&](const char *data, std::size_t length)
                {
                    auto input_state = start_execution_session();
                    (void)input_state;
                    if (execution_session)
                    {
                        execution_session->append_request_body_bytes(data, length);
                    }
                    else if (!request_executor_)
                    {
                        request_context.request_body.append(data, length);
                    }
                },
                std::move(head_read_result.trailing_bytes));
        }
    }
    catch (const QueueCapacityError &)
    {
        return RequestProcessingResult{RequestProcessingOutcome::close, "", false};
    }
    catch (const BodyReadIncompleteError &error)
    {
        Vajra::runtime::note_worker_request_body_time(elapsed_nanoseconds(body_started_at));
        if (execution_session)
        {
            execution_session->fail_request_body("request body stream closed before completion");
            try
            {
                (void)execution_session->finish();
            }
            catch (...)
            {
            }
        }
        emit_native_request_observability(
            access_event_for(request_context, 0, 0, request_started_at, "close"),
            "request_body_error",
            "request_body_incomplete",
            false,
            error.what());
        return RequestProcessingResult{RequestProcessingOutcome::close, "", false};
    }
    catch (const RequestTimeoutError &error)
    {
        Vajra::runtime::note_worker_request_body_time(elapsed_nanoseconds(body_started_at));
        if (execution_session)
        {
            execution_session->fail_request_body("request body timed out before completion");
            try
            {
                (void)execution_session->finish();
            }
            catch (...)
            {
            }
        }
        const auto rejection_response = response_writer_.request_timeout_response();
        const bool response_sent = reject_request_timeout(connection, error, rejection_response);
        const auto event = access_event_for(
            request_context,
            rejection_response.status.code,
            Vajra::response::response_body_size(rejection_response),
            request_started_at,
            "close");
        emit_native_request_observability(event, "request_timeout", "request_body_timeout", response_sent, error.what());
        log_access_event_if_enabled(event);
        return RequestProcessingResult{RequestProcessingOutcome::close, "", false};
    }
    catch (const std::exception &error)
    {
        Vajra::runtime::note_worker_request_body_time(elapsed_nanoseconds(body_started_at));
        if (execution_session)
        {
            execution_session->fail_request_body("request body stream closed before completion");
            try
            {
                (void)execution_session->finish();
            }
            catch (...)
            {
            }
        }
        const auto rejection_response = response_writer_.request_head_failure_response(HeadFailureKind::bad_request);
        const bool response_sent = reject_request_execution(connection, error, rejection_response);
        const auto event = access_event_for(
            request_context,
            rejection_response.status.code,
            Vajra::response::response_body_size(rejection_response),
            request_started_at,
            "close");
        emit_native_request_observability(event, "request_body_error", "bad_request", response_sent, error.what());
        log_access_event_if_enabled(event);
        return RequestProcessingResult{RequestProcessingOutcome::close, "", false};
    }

    Vajra::runtime::note_worker_request_body_time(elapsed_nanoseconds(body_started_at));

    if (execution_session)
    {
        execution_session->finish_request_body();
    }

    Vajra::response::Response response;
    try
    {
        const auto rack_finish_started_at = std::chrono::steady_clock::now();
        if (execution_session)
        {
            response = response_for(*execution_session, connection_behavior);
        }
        else
        {
            response = response_for(request_context, connection_behavior);
        }
        Vajra::runtime::note_worker_rack_finish_time(elapsed_nanoseconds(rack_finish_started_at));
        if (!response.hijacked)
        {
            Vajra::response::ResponseSerializer().validate(response);
        }
    }
    catch (const HeadError &error)
    {
        const auto rejection_response = response_writer_.request_head_failure_response(error.kind());
        const bool response_sent = reject_request_head(connection, error, rejection_response);
        const auto event = access_event_for(
            request_context,
            rejection_response.status.code,
            Vajra::response::response_body_size(rejection_response),
            request_started_at,
            "close");
        emit_native_request_observability(
            event,
            "request_head_error",
            head_failure_kind_token(error.kind()),
            response_sent,
            error.what());
        log_access_event_if_enabled(event);
        return RequestProcessingResult{RequestProcessingOutcome::close, "", false};
    }
    catch (const QueueCapacityError &error)
    {
        const auto rejection_response = response_writer_.queue_capacity_response();
        const bool response_sent = reject_request_queue_capacity(connection, error, rejection_response);
        const auto event = access_event_for(
            request_context,
            rejection_response.status.code,
            Vajra::response::response_body_size(rejection_response),
            request_started_at,
            "close");
        emit_native_request_observability(event, "queue_capacity", "queue_capacity", response_sent, error.what());
        log_access_event_if_enabled(event);
        return RequestProcessingResult{RequestProcessingOutcome::close, "", false};
    }
    catch (const RequestTimeoutError &error)
    {
        const auto rejection_response = response_writer_.request_timeout_response();
        const bool response_sent = reject_request_timeout(connection, error, rejection_response);
        const auto event = access_event_for(
            request_context,
            rejection_response.status.code,
            Vajra::response::response_body_size(rejection_response),
            request_started_at,
            "close");
        emit_native_request_observability(event, "request_timeout", "request_timeout", response_sent, error.what());
        log_access_event_if_enabled(event);
        return RequestProcessingResult{RequestProcessingOutcome::close, "", false};
    }
    catch (const std::exception &error)
    {
        const auto rejection_response = response_writer_.internal_server_error_response();
        const bool response_sent = reject_request_execution(connection, error, rejection_response);
        const auto event = access_event_for(
            request_context,
            rejection_response.status.code,
            Vajra::response::response_body_size(rejection_response),
            request_started_at,
            "close");
        emit_native_request_observability(event, "execution_error", "execution_error", response_sent, error.what());
        log_access_event_if_enabled(event);
        return RequestProcessingResult{RequestProcessingOutcome::close, "", false};
    }

    if (response.connection_behavior == Vajra::response::ConnectionBehavior::close)
    {
        connection_behavior = Vajra::response::ConnectionBehavior::close;
    }

    if (response.hijacked)
    {
        Vajra::runtime::note_worker_request_completed();
        const auto event = access_event_for(
            request_context,
            response.status.code,
            0,
            request_started_at,
            "hijacked");
        log_access_event_if_enabled(event);
        emit_native_request_observability(event, "ok", "", true, "");
        return RequestProcessingResult{RequestProcessingOutcome::hijacked, "", false};
    }

    const InternalTraceResponseHeaders trace_headers = internal_trace_response_headers(response);
    response.headers = Vajra::response::strip_framing_headers(response.headers);
    strip_internal_trace_headers(response);
    const bool suppress_response_body =
        Vajra::request::ascii_case_insensitive_equal(
            request_context.request.request_line.method,
            "HEAD");
    if (!response_writer_.send(connection, response, suppress_response_body))
    {
        return RequestProcessingResult{RequestProcessingOutcome::close, "", false};
    }

    Vajra::runtime::note_worker_request_completed();
    const auto event = access_event_for(
        request_context,
        response.status.code,
        Vajra::response::response_body_size(response),
        request_started_at,
        connection_outcome_for(connection_behavior),
        trace_headers.trace_id,
        trace_headers.span_id);
    log_access_event_if_enabled(event);
    emit_native_request_observability(event, "ok", "", true, "");

    return RequestProcessingResult{
        connection_behavior == Vajra::response::ConnectionBehavior::keep_alive ? RequestProcessingOutcome::keep_alive : RequestProcessingOutcome::close,
        std::move(body_read_result.remaining_buffered_bytes),
        false};
}

Vajra::response::ConnectionBehavior Vajra::request::RequestProcessor::connection_behavior_for(const ParsedRequest &request) const
{
    bool saw_content_length = false;
    const bool http_1_0 = request.request_line.version == "HTTP/1.0";
    bool http_1_0_keep_alive = false;

    for (const ParsedHeader &header : request.headers)
    {
        if (header_named(header, "Connection"))
        {
            if (header_value_contains_token(header.value, "close") || header_value_contains_token(header.value, "upgrade"))
            {
                return Vajra::response::ConnectionBehavior::close;
            }
            if (http_1_0 && header_value_contains_token(header.value, "keep-alive"))
            {
                http_1_0_keep_alive = true;
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

    if (http_1_0 && !http_1_0_keep_alive)
    {
        return Vajra::response::ConnectionBehavior::close;
    }

    return Vajra::response::ConnectionBehavior::keep_alive;
}

Vajra::response::Response Vajra::request::RequestProcessor::response_for(
    RequestExecutionSession &execution_session,
    Vajra::response::ConnectionBehavior connection_behavior) const
{
    const std::optional<Vajra::response::Response> response = execution_session.finish();
    if (!response)
    {
        return response_writer_.success_response(connection_behavior);
    }
    auto result = *response;
    result.headers = Vajra::response::strip_framing_headers(result.headers);
    result.connection_behavior = connection_behavior;
    return result;
}

Vajra::response::Response Vajra::request::RequestProcessor::response_for(
    RequestContext &request_context,
    Vajra::response::ConnectionBehavior connection_behavior) const
{
    if (!request_executor_)
    {
        return response_writer_.success_response(connection_behavior);
    }

    std::optional<Vajra::response::Response> response;
    try
    {
        response = request_executor_->execute(std::move(request_context));
    }
    catch (const std::runtime_error &error)
    {
        if (std::string(error.what()) == "same-process Rack execution pool is closed")
        {
            return response_writer_.success_response(connection_behavior);
        }
        throw;
    }
    if (!response)
    {
        return response_writer_.success_response(connection_behavior);
    }
    auto result = *response;
    result.headers = Vajra::response::strip_framing_headers(result.headers);
    result.connection_behavior = connection_behavior;
    return result;
}

bool Vajra::request::RequestProcessor::reject_request_head(
    Vajra::transport::Connection &connection,
    const HeadError &error,
    const Vajra::response::Response &response) const
{
    response_writer_.log_request_head_error(error);
    return response_writer_.send(connection, response);
}

bool Vajra::request::RequestProcessor::reject_request_queue_capacity(
    Vajra::transport::Connection &connection,
    const QueueCapacityError &error,
    const Vajra::response::Response &response) const
{
    std::ostringstream message;
    message << "request queue capacity reached: client_fd=" << connection.fd() << " error=" << error.what();
    Vajra::runtime::log_runtime_error(message.str());
    return response_writer_.send(connection, response);
}

bool Vajra::request::RequestProcessor::reject_request_timeout(
    Vajra::transport::Connection &connection,
    const RequestTimeoutError &error,
    const Vajra::response::Response &response) const
{
    std::ostringstream message;
    message << "request timed out: client_fd=" << connection.fd() << " error=" << error.what();
    Vajra::runtime::log_runtime_error(message.str());
    return response_writer_.send(connection, response);
}

bool Vajra::request::RequestProcessor::reject_request_execution(
    Vajra::transport::Connection &connection,
    const std::exception &error,
    const Vajra::response::Response &response) const
{
    std::ostringstream message;
    message << "request execution failed: client_fd=" << connection.fd() << " error=" << error.what();
    Vajra::runtime::log_runtime_error(message.str());
    return response_writer_.send(connection, response);
}
