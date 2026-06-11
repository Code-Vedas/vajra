// Copyright Codevedas Inc. 2025-present
//
// This source code is licensed under the MIT license found in the
// LICENSE file in the root directory of this source tree.

#include "request/request_head_size_validator.hpp"
#include "request/request_body_reader.hpp"
#include "request/request_processor.hpp"
#include "response/response_serializer.hpp"
#include "response/response_writer.hpp"
#include "runtime/runtime_logging.hpp"
#include "test_suites.hpp"
#include "test_support.hpp"

#include <cstdio>
#include <fstream>
#include <string>
#include <sys/socket.h>
#include <thread>
#include <unistd.h>

namespace VajraSpecCpp
{
  namespace
  {
    class RaisingRequestExecutor final : public Vajra::request::RequestExecutor
    {
    public:
      std::optional<Vajra::response::Response> execute(const Vajra::request::RequestContext &) const override
      {
        throw std::runtime_error("boom");
      }
    };

    class HeadErrorRequestExecutor final : public Vajra::request::RequestExecutor
    {
    public:
      std::optional<Vajra::response::Response> execute(const Vajra::request::RequestContext &) const override
      {
        throw Vajra::request::bad_request_error("invalid Rack environment translation");
      }
    };

    class InvalidResponseRequestExecutor final : public Vajra::request::RequestExecutor
    {
    public:
      std::optional<Vajra::response::Response> execute(const Vajra::request::RequestContext &) const override
      {
        return Vajra::response::Response{
            Vajra::response::Status{200, "OK"},
            {Vajra::response::Header{"Bad Header", "value"}},
            "OK",
            Vajra::response::ConnectionBehavior::close};
      }
    };

    class FramingHeadersRequestExecutor final : public Vajra::request::RequestExecutor
    {
    public:
      std::optional<Vajra::response::Response> execute(const Vajra::request::RequestContext &) const override
      {
        return Vajra::response::Response{
            Vajra::response::Status{200, "OK"},
            {
                Vajra::response::Header{"Content-Type", "text/plain"},
                Vajra::response::Header{"Content-Length", "999"},
                Vajra::response::Header{"Connection", "keep-alive"},
                Vajra::response::Header{"Transfer-Encoding", "chunked"},
            },
            "OK",
            Vajra::response::ConnectionBehavior::close};
      }
    };

    class EchoRequestBodyExecutor final : public Vajra::request::RequestExecutor
    {
    public:
      std::optional<Vajra::response::Response> execute(const Vajra::request::RequestContext &request_context) const override
      {
        return Vajra::response::Response{
            Vajra::response::Status{200, "OK"},
            {Vajra::response::Header{"Content-Type", "application/octet-stream"}},
            request_context.request_body,
            Vajra::response::ConnectionBehavior::close};
      }
    };

    class TraceHeaderRequestExecutor final : public Vajra::request::RequestExecutor
    {
    public:
      std::optional<Vajra::response::Response> execute(const Vajra::request::RequestContext &) const override
      {
        return Vajra::response::Response{
            Vajra::response::Status{200, "OK"},
            {
                Vajra::response::Header{"Content-Type", "text/plain"},
                Vajra::response::Header{"X-Vajra-Internal-Trace-Id", "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"},
            },
            "OK",
            Vajra::response::ConnectionBehavior::close};
      }
    };

    std::thread start_request_processor_thread(
        const Vajra::request::RequestProcessor &processor,
        FileDescriptorGuard &server_socket)
    {
      const int owned_fd = dup(server_socket.get());
      if (owned_fd < 0)
      {
        fail("dup failed while transferring request processor socket ownership");
      }

      server_socket.close_if_open();
      return std::thread([&processor, owned_fd]() {
        processor.handle(owned_fd, Vajra::request::SocketContext{"127.0.0.1", 12'345, "127.0.0.1", 3000, "http"});
      });
    }

    void expect_serialization_error(
        const Vajra::response::Response &response,
        const std::string &expected_message)
    {
      try
      {
        Vajra::response::ResponseSerializer serializer;
        (void)serializer.serialize(response);
      }
      catch (const Vajra::response::SerializationError &error)
      {
        if (std::string(error.what()).find(expected_message) == std::string::npos)
        {
          fail("response serializer raised the wrong validation error");
        }

        return;
      }

      fail("response serializer accepted an invalid response");
    }

    void test_response_serializer_serializes_status_headers_and_body()
    {
      Vajra::response::ResponseSerializer serializer;
      const Vajra::response::Response response{
          Vajra::response::Status{201, "Created"},
          {
              Vajra::response::Header{"Content-Type", "text/plain"},
              Vajra::response::Header{"X-Trace-Id", "trace-123"},
          },
          "done",
          Vajra::response::ConnectionBehavior::keep_alive};

      const std::string serialized = serializer.serialize(response);
      const std::string expected =
          "HTTP/1.1 201 Created\r\n"
          "Content-Type: text/plain\r\n"
          "X-Trace-Id: trace-123\r\n"
          "Content-Length: 4\r\n"
          "\r\n"
          "done";

      if (serialized != expected)
      {
        fail("response serializer did not produce the expected wire format");
      }
    }

    void test_response_serializer_allows_empty_reason_phrase()
    {
      Vajra::response::ResponseSerializer serializer;
      const std::string serialized = serializer.serialize(
          Vajra::response::Response{
              Vajra::response::Status{200, ""},
              {Vajra::response::Header{"Content-Type", "text/plain"}},
              "OK"});

      if (serialized.find("HTTP/1.1 200 \r\n") != 0)
      {
        fail("response serializer did not preserve an empty reason phrase");
      }
    }

    void test_response_serializer_omits_content_length_for_no_body_status()
    {
      Vajra::response::ResponseSerializer serializer;
      const Vajra::response::Response response{
          Vajra::response::Status{204, "No Content"},
          {Vajra::response::Header{"Content-Type", "text/plain"}},
          ""};

      const std::string serialized = serializer.serialize(response);
      if (serialized.find("Content-Length:") != std::string::npos)
      {
        fail("no-body statuses must not emit content length framing");
      }

      if (serialized.find("\r\n\r\n") == std::string::npos)
      {
        fail("no-body statuses did not terminate headers correctly");
      }
    }

    void test_response_serializer_emits_connection_close_only_when_requested()
    {
      Vajra::response::ResponseSerializer serializer;
      const std::string close_serialized = serializer.serialize(
          Vajra::response::Response{
              Vajra::response::Status{200, "OK"},
              {Vajra::response::Header{"Content-Type", "text/plain"}},
              "OK",
              Vajra::response::ConnectionBehavior::close});

      if (close_serialized.find("Connection: close\r\n") == std::string::npos)
      {
        fail("response serializer omitted the close framing header");
      }

      const std::string keep_alive_serialized = serializer.serialize(
          Vajra::response::Response{
              Vajra::response::Status{200, "OK"},
              {Vajra::response::Header{"Content-Type", "text/plain"}},
              "OK",
              Vajra::response::ConnectionBehavior::keep_alive});

      if (keep_alive_serialized.find("Connection: close\r\n") != std::string::npos)
      {
        fail("response serializer emitted close framing for a reusable response");
      }
    }

    void test_response_serializer_preserves_header_order()
    {
      Vajra::response::ResponseSerializer serializer;
      const Vajra::response::Response response{
          Vajra::response::Status{200, "OK"},
          {
              Vajra::response::Header{"X-First", "1"},
              Vajra::response::Header{"X-Second", "2"},
              Vajra::response::Header{"X-Third", "3"},
          },
          "OK"};

      const std::string serialized = serializer.serialize(response);
      const std::size_t first = serialized.find("X-First: 1\r\n");
      const std::size_t second = serialized.find("X-Second: 2\r\n");
      const std::size_t third = serialized.find("X-Third: 3\r\n");

      if (first == std::string::npos || second == std::string::npos || third == std::string::npos)
      {
        fail("response serializer omitted headers");
      }

      if (!(first < second && second < third))
      {
        fail("response serializer did not preserve header order");
      }
    }

    void test_response_serializer_rejects_invalid_header_name()
    {
      expect_serialization_error(
          Vajra::response::Response{
              Vajra::response::Status{200, "OK"},
              {Vajra::response::Header{"Bad Header", "value"}},
              "OK"},
          "response header name contains an unsafe character");

      expect_serialization_error(
          Vajra::response::Response{
              Vajra::response::Status{200, "OK"},
              {Vajra::response::Header{"X(Header)", "value"}},
              "OK"},
          "response header name contains an unsafe character");

      expect_serialization_error(
          Vajra::response::Response{
              Vajra::response::Status{200, "OK"},
              {Vajra::response::Header{std::string("X-\xFF", 3), "value"}},
              "OK"},
          "response header name contains an unsafe character");
    }

    void test_response_serializer_rejects_invalid_header_value()
    {
      expect_serialization_error(
          Vajra::response::Response{
              Vajra::response::Status{200, "OK"},
              {Vajra::response::Header{"X-Test", "bad\r\nvalue"}},
              "OK"},
          "response header value contains an unsafe control character");
    }

    void test_response_serializer_rejects_forbidden_framing_headers()
    {
      expect_serialization_error(
          Vajra::response::Response{
              Vajra::response::Status{200, "OK"},
              {Vajra::response::Header{"Content-Length", "999"}},
              "OK"},
          "response headers must not override HTTP framing headers");

      expect_serialization_error(
          Vajra::response::Response{
              Vajra::response::Status{200, "OK"},
              {Vajra::response::Header{"Transfer-Encoding", "chunked"}},
              "OK"},
          "response headers must not override HTTP framing headers");
    }

    void test_response_serializer_rejects_other_control_characters()
    {
      expect_serialization_error(
          Vajra::response::Response{
              Vajra::response::Status{200, std::string("O\0K", 3)},
              {Vajra::response::Header{"Content-Type", "text/plain"}},
              "OK"},
          "response reason phrase contains an unsafe control character");

      expect_serialization_error(
          Vajra::response::Response{
              Vajra::response::Status{200, "OK"},
              {Vajra::response::Header{"X-Test", std::string("bad\0value", 9)}},
              "OK"},
          "response header value contains an unsafe control character");
    }

    void test_response_serializer_rejects_invalid_status()
    {
      expect_serialization_error(
          Vajra::response::Response{
              Vajra::response::Status{99, "OK"},
              {Vajra::response::Header{"Content-Type", "text/plain"}},
              "OK"},
          "response status code must be within the HTTP/1.1 range 100-599");

      expect_serialization_error(
          Vajra::response::Response{
              Vajra::response::Status{600, "Custom"},
              {Vajra::response::Header{"Content-Type", "text/plain"}},
              "OK"},
          "response status code must be within the HTTP/1.1 range 100-599");
    }

    void test_response_serializer_rejects_bodies_for_no_body_statuses()
    {
      expect_serialization_error(
          Vajra::response::Response{
              Vajra::response::Status{204, "No Content"},
              {Vajra::response::Header{"Content-Type", "text/plain"}},
              "not allowed"},
          "response status must not include a message body");

      expect_serialization_error(
          Vajra::response::Response{
              Vajra::response::Status{205, "Reset Content"},
              {Vajra::response::Header{"Content-Type", "text/plain"}},
              "not allowed"},
          "response status must not include a message body");

      Vajra::response::ResponseSerializer serializer;
      const std::string serialized = serializer.serialize(
          Vajra::response::Response{
              Vajra::response::Status{304, "Not Modified"},
              {Vajra::response::Header{"Content-Type", "text/plain"}},
              ""});

      if (serialized.find("Content-Length:") != std::string::npos)
      {
        fail("no-body statuses must not emit content length framing");
      }

      if (serialized.find("\r\n\r\n") == std::string::npos)
      {
        fail("no-body status response did not terminate headers correctly");
      }

      if (serialized.find("\r\n\r\nNot Modified") != std::string::npos)
      {
        fail("no-body statuses must not serialize a payload");
      }

      const std::string reset_content_serialized = serializer.serialize(
          Vajra::response::Response{
              Vajra::response::Status{205, "Reset Content"},
              {Vajra::response::Header{"Content-Type", "text/plain"}},
              ""});

      if (reset_content_serialized.find("Content-Length:") != std::string::npos)
      {
        fail("205 responses must not emit content length framing");
      }
    }

    void test_response_writer_send_returns_false_on_serialization_failure()
    {
      int sockets[2];
      if (socketpair(AF_UNIX, SOCK_STREAM, 0, sockets) < 0)
      {
        fail("socketpair failed while setting up invalid response writer test");
      }

      FileDescriptorGuard reader_socket(sockets[0]);
      FileDescriptorGuard writer_socket(sockets[1]);

      Vajra::response::ResponseWriter writer;
      const bool sent = writer.send(
          writer_socket.get(),
          Vajra::response::Response{
              Vajra::response::Status{200, "OK"},
              {Vajra::response::Header{"Connection", "keep-alive"}},
              "OK"});

      if (sent)
      {
        fail("response writer reported success for an invalid response");
      }

      writer_socket.close_if_open();
      const std::string response = read_all(reader_socket.get());
      if (!response.empty())
      {
        fail("response writer emitted bytes for an invalid response");
      }
    }

    void test_response_writer_sends_structured_success_response()
    {
      Vajra::response::ResponseWriter writer;
      const std::string response =
          send_response_through_socket(writer.success_response(Vajra::response::ConnectionBehavior::keep_alive));

      if (response.find("HTTP/1.1 200 OK\r\n") != 0)
      {
        fail("success response did not start with a 200 OK status line");
      }

      if (response.find("Content-Type: text/plain\r\n") == std::string::npos)
      {
        fail("success response omitted content type");
      }

      if (response.find("Content-Length: 2\r\n") == std::string::npos)
      {
        fail("success response used the wrong content length");
      }

      if (response.find("Connection: close\r\n") != std::string::npos)
      {
        fail("success response unexpectedly forced connection close");
      }

      if (response.find("\r\n\r\nOK") == std::string::npos)
      {
        fail("success response body did not match the serialized payload");
      }
    }

    void test_response_writer_sends_structured_error_response()
    {
      Vajra::response::ResponseWriter writer;
      const std::string response = send_response_through_socket(
          writer.request_head_failure_response(Vajra::request::HeadFailureKind::header_too_large));

      if (response.find("HTTP/1.1 431 Request Header Fields Too Large\r\n") != 0)
      {
        fail("error response did not start with the expected status line");
      }

      if (response.find("Content-Length: 31\r\n") == std::string::npos)
      {
        fail("error response used the wrong content length");
      }

      if (response.find("Connection: close\r\n") == std::string::npos)
      {
        fail("error response did not force connection close");
      }

      if (response.find("\r\n\r\nRequest Header Fields Too Large") == std::string::npos)
      {
        fail("error response body did not match the serialized payload");
      }
    }

    void test_request_processor_keeps_connection_open_for_sequential_requests()
    {
      int sockets[2];
      if (socketpair(AF_UNIX, SOCK_STREAM, 0, sockets) < 0)
      {
        fail("socketpair failed while setting up sequential request processor test");
      }

      FileDescriptorGuard client_socket(sockets[0]);
      FileDescriptorGuard server_socket(sockets[1]);
      suppress_sigpipe(client_socket.get());

      const Vajra::request::RequestProcessor processor(Vajra::request::kDefaultMaxRequestHeadBytes);
      std::thread processor_thread = start_request_processor_thread(processor, server_socket);

      try
      {
        if (!send_all(
                client_socket.get(),
                "GET /first HTTP/1.1\r\n"
                "Host: example.test\r\n"
                "\r\n"))
        {
          fail("failed to send first keep-alive request");
        }

        const std::string first_response = read_http_response(client_socket.get());
        if (first_response.find("HTTP/1.1 200 OK\r\n") != 0)
        {
          fail("first keep-alive response did not succeed");
        }

        if (first_response.find("Connection: close\r\n") != std::string::npos)
        {
          fail("first keep-alive response unexpectedly forced connection close");
        }

        if (!send_all(
                client_socket.get(),
                "POST /zero HTTP/1.1\r\n"
                "Host: example.test\r\n"
                "Content-Length: 000\r\n"
                "\r\n"))
        {
          fail("failed to send zero-valued content length request");
        }

        const std::string zero_length_response = read_http_response(client_socket.get());
        if (zero_length_response.find("HTTP/1.1 200 OK\r\n") != 0)
        {
          fail("zero-valued content length request did not receive a success response");
        }

        if (zero_length_response.find("Connection: close\r\n") != std::string::npos)
        {
          fail("zero-valued content length request unexpectedly forced connection close");
        }

        if (!send_all(
                client_socket.get(),
                "GET /second HTTP/1.1\r\n"
                "Host: example.test\r\n"
                "Connection: close\r\n"
                "\r\n"))
        {
          fail("failed to send second request on a reusable connection");
        }

        const std::string second_response = read_http_response(client_socket.get());
        if (second_response.find("HTTP/1.1 200 OK\r\n") != 0)
        {
          fail("second keep-alive response did not succeed");
        }

        if (second_response.find("Connection: close\r\n") == std::string::npos)
        {
          fail("second response did not advertise connection close");
        }

        if (!peer_closed_within(client_socket.get(), 500))
        {
          fail("request processor did not close the connection after the close response");
        }

        client_socket.close_if_open();
        processor_thread.join();
      }
      catch (...)
      {
        client_socket.close_if_open();
        if (processor_thread.joinable())
        {
          processor_thread.join();
        }
        throw;
      }
    }

    void test_request_processor_handles_pipelined_read_ahead_without_losing_the_next_request()
    {
      int sockets[2];
      if (socketpair(AF_UNIX, SOCK_STREAM, 0, sockets) < 0)
      {
        fail("socketpair failed while setting up pipelined request processor test");
      }

      FileDescriptorGuard client_socket(sockets[0]);
      FileDescriptorGuard server_socket(sockets[1]);
      suppress_sigpipe(client_socket.get());

      const Vajra::request::RequestProcessor processor(Vajra::request::kDefaultMaxRequestHeadBytes);
      std::thread processor_thread = start_request_processor_thread(processor, server_socket);

      try
      {
        if (!send_all(
                client_socket.get(),
                "GET /first HTTP/1.1\r\n"
                "Host: example.test\r\n"
                "\r\n"
                "GET /second HTTP/1.1\r\n"
                "Host: example.test\r\n"
                "Connection: close\r\n"
                "\r\n"))
        {
          fail("failed to send pipelined request bytes");
        }

        const std::string all_responses = read_all(client_socket.get());
        const std::size_t first_response_end = all_responses.find("\r\n\r\nOK");
        if (first_response_end == std::string::npos)
        {
          fail("pipelined responses did not contain the first complete payload");
        }

        const std::string first_response = all_responses.substr(0, first_response_end + 6);
        if (first_response.find("HTTP/1.1 200 OK\r\n") != 0)
        {
          fail("first pipelined response did not succeed");
        }

        if (first_response.find("Connection: close\r\n") != std::string::npos)
        {
          fail("first pipelined response unexpectedly forced connection close");
        }

        const std::string second_response = all_responses.substr(first_response_end + 6);
        if (second_response.find("HTTP/1.1 200 OK\r\n") != 0)
        {
          fail("second pipelined response did not succeed");
        }

        if (second_response.find("Connection: close\r\n") == std::string::npos)
        {
          fail("second pipelined response did not advertise connection close");
        }

        client_socket.close_if_open();
        processor_thread.join();
      }
      catch (...)
      {
        client_socket.close_if_open();
        if (processor_thread.joinable())
        {
          processor_thread.join();
        }
        throw;
      }
    }

    void test_request_processor_closes_after_parse_error_response()
    {
      int sockets[2];
      if (socketpair(AF_UNIX, SOCK_STREAM, 0, sockets) < 0)
      {
        fail("socketpair failed while setting up parse error request processor test");
      }

      FileDescriptorGuard client_socket(sockets[0]);
      FileDescriptorGuard server_socket(sockets[1]);
      suppress_sigpipe(client_socket.get());

      const Vajra::request::RequestProcessor processor(Vajra::request::kDefaultMaxRequestHeadBytes);
      std::thread processor_thread = start_request_processor_thread(processor, server_socket);

      try
      {
        if (!send_all(
                client_socket.get(),
                "GET /only-two-parts\r\n"
                "Host: example.test\r\n"
                "\r\n"))
        {
          fail("failed to send malformed request");
        }

        const std::string response = read_http_response(client_socket.get());
        if (response.find("HTTP/1.1 400 Bad Request\r\n") != 0)
        {
          fail("parse error response did not use 400 Bad Request");
        }

        if (response.find("Connection: close\r\n") == std::string::npos)
        {
          fail("parse error response did not force connection close");
        }

        if (!peer_closed_within(client_socket.get(), 500))
        {
          fail("request processor left the connection open after a parse error");
        }

        client_socket.close_if_open();
        processor_thread.join();
      }
      catch (...)
      {
        client_socket.close_if_open();
        if (processor_thread.joinable())
        {
          processor_thread.join();
        }
        throw;
      }
    }

    void test_request_processor_keeps_connection_open_after_request_body_framing()
    {
      int sockets[2];
      if (socketpair(AF_UNIX, SOCK_STREAM, 0, sockets) < 0)
      {
        fail("socketpair failed while setting up request body framing test");
      }

      FileDescriptorGuard client_socket(sockets[0]);
      FileDescriptorGuard server_socket(sockets[1]);
      suppress_sigpipe(client_socket.get());

      const auto request_executor = std::make_shared<EchoRequestBodyExecutor>();
      const Vajra::request::RequestProcessor processor(
          Vajra::request::kDefaultMaxRequestHeadBytes,
          request_executor);
      std::thread processor_thread = start_request_processor_thread(processor, server_socket);

      try
      {
        if (!send_all(
                client_socket.get(),
                "POST / HTTP/1.1\r\n"
                "Host: example.test\r\n"
                "Content-Length: 4\r\n"
                "\r\n"
                "body"))
        {
          fail("failed to send request with body framing");
        }

        const std::string response = read_http_response(client_socket.get());
        if (response.find("HTTP/1.1 200 OK\r\n") != 0)
        {
          fail("request with body framing did not receive the success response");
        }

        if (response.size() < 4 || response.compare(response.size() - 4, 4, "body") != 0)
        {
          fail("request processor did not transport the fixed-length request body");
        }

        if (response.find("Connection: close\r\n") != std::string::npos)
        {
          fail("request with body framing unexpectedly forced connection close");
        }

        if (!send_all(
                client_socket.get(),
                "POST /second HTTP/1.1\r\n"
                "Host: example.test\r\n"
                "Content-Length: 6\r\n"
                "Connection: close\r\n"
                "\r\n"
                "second"))
        {
          fail("failed to send second request after request body framing");
        }

        const std::string second_response = read_http_response(client_socket.get());
        if (second_response.find("HTTP/1.1 200 OK\r\n") != 0)
        {
          fail("second request with body framing did not receive the success response");
        }

        if (second_response.size() < 6 || second_response.compare(second_response.size() - 6, 6, "second") != 0)
        {
          fail("request processor did not transport the second fixed-length request body");
        }

        if (second_response.find("Connection: close\r\n") == std::string::npos)
        {
          fail("second request with explicit close did not advertise connection close");
        }

        if (!peer_closed_within(client_socket.get(), 500))
        {
          fail("request processor left the connection open after explicit close");
        }

        client_socket.close_if_open();
        processor_thread.join();
      }
      catch (...)
      {
        client_socket.close_if_open();
        if (processor_thread.joinable())
        {
          processor_thread.join();
        }
        throw;
      }
    }

    void test_request_body_reader_preserves_buffered_suffix_after_fixed_length_body()
    {
      Vajra::request::RequestBodyReader reader;
      const Vajra::request::ParsedRequest request{
          Vajra::request::ParsedRequestLine{"POST", "/", "HTTP/1.1"},
          {Vajra::request::ParsedHeader{"Content-Length", "4"}}};

      const Vajra::request::BodyReadResult result = reader.read(
          -1,
          request,
          "bodyGET /next HTTP/1.1\r\nHost: example.test\r\n\r\n");

      if (result.body != "body")
      {
        fail("fixed-length body reader did not preserve the request body");
      }

      if (result.remaining_buffered_bytes != "GET /next HTTP/1.1\r\nHost: example.test\r\n\r\n")
      {
        fail("fixed-length body reader did not preserve unread buffered bytes");
      }
    }

    void test_request_body_reader_preserves_buffered_suffix_after_chunked_body()
    {
      Vajra::request::RequestBodyReader reader;
      const Vajra::request::ParsedRequest request{
          Vajra::request::ParsedRequestLine{"POST", "/", "HTTP/1.1"},
          {Vajra::request::ParsedHeader{"Transfer-Encoding", "chunked"}}};

      const Vajra::request::BodyReadResult result = reader.read(
          -1,
          request,
          "3\r\nabc\r\n0\r\nX-Trailer: done\r\n\r\nGET /next HTTP/1.1\r\nHost: example.test\r\n\r\n");

      if (result.body != "abc")
      {
        fail("chunked body reader did not decode the request body");
      }

      if (result.remaining_buffered_bytes != "GET /next HTTP/1.1\r\nHost: example.test\r\n\r\n")
      {
        fail("chunked body reader did not preserve unread buffered bytes");
      }
    }

    void test_request_body_reader_rejects_oversized_content_length_body()
    {
      const Vajra::request::RequestBodyReader reader(4);
      const Vajra::request::ParsedRequest request{
          Vajra::request::ParsedRequestLine{"POST", "/", "HTTP/1.1"},
          {Vajra::request::ParsedHeader{"Content-Length", "5"}}};

      try
      {
        (void)reader.read(-1, request, "");
      }
      catch (const Vajra::request::HeadError &error)
      {
        if (error.kind() != Vajra::request::HeadFailureKind::bad_request ||
            std::string(error.what()).find("request body exceeds maximum size") == std::string::npos)
        {
          fail("oversized content-length body raised the wrong error");
        }

        return;
      }

      fail("oversized content-length body was accepted");
    }

    void test_request_body_reader_rejects_overlong_chunk_metadata()
    {
      const Vajra::request::RequestBodyReader reader(
          Vajra::request::kDefaultMaxRequestBodyBytes,
          4,
          Vajra::request::kDefaultMaxTrailerLineBytes);
      const Vajra::request::ParsedRequest request{
          Vajra::request::ParsedRequestLine{"POST", "/", "HTTP/1.1"},
          {Vajra::request::ParsedHeader{"Transfer-Encoding", "chunked"}}};

      try
      {
        (void)reader.read(-1, request, "12345");
      }
      catch (const Vajra::request::HeadError &error)
      {
        if (error.kind() != Vajra::request::HeadFailureKind::bad_request ||
            std::string(error.what()).find("request body metadata exceeds maximum size") == std::string::npos)
        {
          fail("overlong chunk metadata raised the wrong error");
        }

        return;
      }

      fail("overlong chunk metadata was accepted");
    }

    void test_request_body_reader_treats_chunk_metadata_at_limit_without_crlf_as_incomplete()
    {
      const Vajra::request::RequestBodyReader reader(
          Vajra::request::kDefaultMaxRequestBodyBytes,
          4,
          Vajra::request::kDefaultMaxTrailerLineBytes);
      const Vajra::request::ParsedRequest request{
          Vajra::request::ParsedRequestLine{"POST", "/", "HTTP/1.1"},
          {Vajra::request::ParsedHeader{"Transfer-Encoding", "chunked"}}};

      try
      {
        (void)reader.read(-1, request, "1234");
      }
      catch (const Vajra::request::BodyReadIncompleteError &)
      {
        return;
      }

      fail("chunk metadata at the limit was not treated as an incomplete read");
    }

    void test_request_body_reader_treats_transport_failures_as_incomplete_reads()
    {
      Vajra::request::RequestBodyReader reader;
      const Vajra::request::ParsedRequest request{
          Vajra::request::ParsedRequestLine{"POST", "/", "HTTP/1.1"},
          {Vajra::request::ParsedHeader{"Content-Length", "4"}}};

      try
      {
        (void)reader.read(-1, request, "");
      }
      catch (const Vajra::request::BodyReadIncompleteError &)
      {
        return;
      }
      catch (const Vajra::request::HeadError &)
      {
        fail("transport failure was misclassified as invalid request body framing");
      }

      fail("transport failure did not raise an incomplete body read");
    }

    void test_request_processor_reads_fragmented_fixed_length_request_body()
    {
      int sockets[2];
      if (socketpair(AF_UNIX, SOCK_STREAM, 0, sockets) < 0)
      {
        fail("socketpair failed while setting up fragmented fixed-length body test");
      }

      FileDescriptorGuard client_socket(sockets[0]);
      FileDescriptorGuard server_socket(sockets[1]);
      suppress_sigpipe(client_socket.get());

      const auto request_executor = std::make_shared<EchoRequestBodyExecutor>();
      const Vajra::request::RequestProcessor processor(
          Vajra::request::kDefaultMaxRequestHeadBytes,
          request_executor);
      std::thread processor_thread = start_request_processor_thread(processor, server_socket);

      try
      {
        if (!send_all(
                client_socket.get(),
                "POST /fragmented HTTP/1.1\r\n"
                "Host: example.test\r\n"
                "Content-Length: 11\r\n"
                "\r\n"
                "hello"))
        {
          fail("failed to send fixed-length body prefix");
        }

        if (!send_all(client_socket.get(), " world"))
        {
          fail("failed to send fixed-length body suffix");
        }

        const std::string response = read_http_response(client_socket.get());
        if (response.substr(response.size() - 11) != "hello world")
        {
          fail("fragmented fixed-length request body was not reassembled");
        }

        client_socket.close_if_open();
        processor_thread.join();
      }
      catch (...)
      {
        client_socket.close_if_open();
        if (processor_thread.joinable())
        {
          processor_thread.join();
        }
        throw;
      }
    }

    void test_request_processor_decodes_chunked_request_body()
    {
      int sockets[2];
      if (socketpair(AF_UNIX, SOCK_STREAM, 0, sockets) < 0)
      {
        fail("socketpair failed while setting up chunked body test");
      }

      FileDescriptorGuard client_socket(sockets[0]);
      FileDescriptorGuard server_socket(sockets[1]);
      suppress_sigpipe(client_socket.get());

      const auto request_executor = std::make_shared<EchoRequestBodyExecutor>();
      const Vajra::request::RequestProcessor processor(
          Vajra::request::kDefaultMaxRequestHeadBytes,
          request_executor);
      std::thread processor_thread = start_request_processor_thread(processor, server_socket);

      try
      {
        if (!send_all(
                client_socket.get(),
                "POST /chunked HTTP/1.1\r\n"
                "Host: example.test\r\n"
                "Transfer-Encoding: chunked\r\n"
                "\r\n"
                "3\r\nabc\r\n"
                "6\r\n123456\r\n"
                "0\r\n\r\n"))
        {
          fail("failed to send chunked request body");
        }

        const std::string response = read_http_response(client_socket.get());
        if (response.substr(response.size() - 9) != "abc123456")
        {
          fail("chunked request body was not decoded correctly");
        }

        client_socket.close_if_open();
        processor_thread.join();
      }
      catch (...)
      {
        client_socket.close_if_open();
        if (processor_thread.joinable())
        {
          processor_thread.join();
        }
        throw;
      }
    }

    void test_request_processor_decodes_chunked_request_body_with_extensions_and_trailers()
    {
      int sockets[2];
      if (socketpair(AF_UNIX, SOCK_STREAM, 0, sockets) < 0)
      {
        fail("socketpair failed while setting up chunked extension test");
      }

      FileDescriptorGuard client_socket(sockets[0]);
      FileDescriptorGuard server_socket(sockets[1]);
      suppress_sigpipe(client_socket.get());

      const auto request_executor = std::make_shared<EchoRequestBodyExecutor>();
      const Vajra::request::RequestProcessor processor(
          Vajra::request::kDefaultMaxRequestHeadBytes,
          request_executor);
      std::thread processor_thread = start_request_processor_thread(processor, server_socket);

      try
      {
        if (!send_all(
                client_socket.get(),
                "POST /chunked-extensions HTTP/1.1\r\n"
                "Host: example.test\r\n"
                "Transfer-Encoding: chunked\r\n"
                "\r\n"
                "5;foo=bar\r\nhello\r\n"
                "1\r\n \r\n"
                "0\r\n"
                "X-Trailer: kept-native-only\r\n"
                "\r\n"))
        {
          fail("failed to send chunked request with extensions and trailers");
        }

        const std::string response = read_http_response(client_socket.get());
        if (response.substr(response.size() - 6) != "hello ")
        {
          fail("chunked body extensions or trailers were not handled correctly");
        }

        client_socket.close_if_open();
        processor_thread.join();
      }
      catch (...)
      {
        client_socket.close_if_open();
        if (processor_thread.joinable())
        {
          processor_thread.join();
        }
        throw;
      }
    }

    void test_request_processor_rejects_conflicting_request_body_framing()
    {
      int sockets[2];
      if (socketpair(AF_UNIX, SOCK_STREAM, 0, sockets) < 0)
      {
        fail("socketpair failed while setting up conflicting request body framing test");
      }

      FileDescriptorGuard client_socket(sockets[0]);
      FileDescriptorGuard server_socket(sockets[1]);
      suppress_sigpipe(client_socket.get());

      const Vajra::request::RequestProcessor processor(Vajra::request::kDefaultMaxRequestHeadBytes);
      std::thread processor_thread = start_request_processor_thread(processor, server_socket);

      try
      {
        if (!send_all(
                client_socket.get(),
                "POST /conflict HTTP/1.1\r\n"
                "Host: example.test\r\n"
                "Content-Length: 3\r\n"
                "Transfer-Encoding: chunked\r\n"
                "\r\n"
                "3\r\nabc\r\n0\r\n\r\n"))
        {
          fail("failed to send conflicting request body framing");
        }

        const std::string response = read_http_response(client_socket.get());
        if (response.find("HTTP/1.1 400 Bad Request\r\n") != 0)
        {
          fail("conflicting request body framing did not receive a 400 response");
        }

        client_socket.close_if_open();
        processor_thread.join();
      }
      catch (...)
      {
        client_socket.close_if_open();
        if (processor_thread.joinable())
        {
          processor_thread.join();
        }
        throw;
      }
    }

    void test_request_processor_rejects_malformed_chunked_request_body()
    {
      int sockets[2];
      if (socketpair(AF_UNIX, SOCK_STREAM, 0, sockets) < 0)
      {
        fail("socketpair failed while setting up malformed chunked body test");
      }

      FileDescriptorGuard client_socket(sockets[0]);
      FileDescriptorGuard server_socket(sockets[1]);
      suppress_sigpipe(client_socket.get());

      const Vajra::request::RequestProcessor processor(Vajra::request::kDefaultMaxRequestHeadBytes);
      std::thread processor_thread = start_request_processor_thread(processor, server_socket);

      try
      {
        if (!send_all(
                client_socket.get(),
                "POST /malformed-chunked HTTP/1.1\r\n"
                "Host: example.test\r\n"
                "Transfer-Encoding: chunked\r\n"
                "\r\n"
                "Z\r\nabc\r\n0\r\n\r\n"))
        {
          fail("failed to send malformed chunked request body");
        }

        const std::string response = read_http_response(client_socket.get());
        if (response.find("HTTP/1.1 400 Bad Request\r\n") != 0)
        {
          fail("malformed chunked request body did not receive a 400 response");
        }

        client_socket.close_if_open();
        processor_thread.join();
      }
      catch (...)
      {
        client_socket.close_if_open();
        if (processor_thread.joinable())
        {
          processor_thread.join();
        }
        throw;
      }
    }

    void test_request_processor_rejects_oversized_request_body()
    {
      int sockets[2];
      if (socketpair(AF_UNIX, SOCK_STREAM, 0, sockets) < 0)
      {
        fail("socketpair failed while setting up oversized request body test");
      }

      FileDescriptorGuard client_socket(sockets[0]);
      FileDescriptorGuard server_socket(sockets[1]);
      suppress_sigpipe(client_socket.get());

      const Vajra::request::RequestProcessor processor(Vajra::request::kDefaultMaxRequestHeadBytes);
      std::thread processor_thread = start_request_processor_thread(processor, server_socket);

      try
      {
        if (!send_all(
                client_socket.get(),
                "POST /too-large HTTP/1.1\r\n"
                "Host: example.test\r\n"
                "Content-Length: 16777217\r\n"
                "\r\n"))
        {
          fail("failed to send oversized request body framing");
        }

        const std::string response = read_http_response(client_socket.get());
        if (response.find("HTTP/1.1 400 Bad Request\r\n") != 0)
        {
          fail("oversized request body did not receive a 400 response");
        }

        client_socket.close_if_open();
        processor_thread.join();
      }
      catch (...)
      {
        client_socket.close_if_open();
        if (processor_thread.joinable())
        {
          processor_thread.join();
        }
        throw;
      }
    }

    void test_request_processor_closes_quietly_when_request_body_is_incomplete()
    {
      int sockets[2];
      if (socketpair(AF_UNIX, SOCK_STREAM, 0, sockets) < 0)
      {
        fail("socketpair failed while setting up incomplete request body test");
      }

      FileDescriptorGuard client_socket(sockets[0]);
      FileDescriptorGuard server_socket(sockets[1]);
      suppress_sigpipe(client_socket.get());

      const auto request_executor = std::make_shared<EchoRequestBodyExecutor>();
      const Vajra::request::RequestProcessor processor(
          Vajra::request::kDefaultMaxRequestHeadBytes,
          request_executor);
      std::thread processor_thread = start_request_processor_thread(processor, server_socket);

      try
      {
        if (!send_all(
                client_socket.get(),
                "POST /partial HTTP/1.1\r\n"
                "Host: example.test\r\n"
                "Content-Length: 8\r\n"
                "\r\n"
                "body"))
        {
          fail("failed to send partial request body");
        }

        if (shutdown(client_socket.get(), SHUT_WR) < 0)
        {
          fail("failed to half-close partial request body socket");
        }

        if (!peer_closed_within(client_socket.get(), 500))
        {
          fail("request processor did not close after an incomplete request body");
        }

        const std::string response = read_all(client_socket.get());
        if (!response.empty())
        {
          fail("incomplete request body unexpectedly produced a response");
        }

        client_socket.close_if_open();
        processor_thread.join();
      }
      catch (...)
      {
        client_socket.close_if_open();
        if (processor_thread.joinable())
        {
          processor_thread.join();
        }
        throw;
      }
    }

    void test_request_processor_returns_internal_server_error_when_executor_raises()
    {
      int sockets[2];
      if (socketpair(AF_UNIX, SOCK_STREAM, 0, sockets) < 0)
      {
        fail("socketpair failed while setting up executor failure test");
      }

      FileDescriptorGuard client_socket(sockets[0]);
      FileDescriptorGuard server_socket(sockets[1]);
      suppress_sigpipe(client_socket.get());

      const auto request_executor = std::make_shared<RaisingRequestExecutor>();
      const Vajra::request::RequestProcessor processor(
          Vajra::request::kDefaultMaxRequestHeadBytes,
          request_executor);
      std::thread processor_thread = start_request_processor_thread(processor, server_socket);

      try
      {
        if (!send_all(
                client_socket.get(),
                "GET /boom HTTP/1.1\r\n"
                "Host: example.test\r\n"
                "Connection: close\r\n"
                "\r\n"))
        {
          fail("failed to send request for executor failure test");
        }

        const std::string response = read_http_response(client_socket.get());
        if (response.find("HTTP/1.1 500 Internal Server Error\r\n") != 0)
        {
          fail("executor failure did not return a 500 response");
        }

        if (response.find("Connection: close\r\n") == std::string::npos)
        {
          fail("executor failure did not force connection close");
        }

        client_socket.close_if_open();
        processor_thread.join();
      }
      catch (...)
      {
        client_socket.close_if_open();
        if (processor_thread.joinable())
        {
          processor_thread.join();
        }
        throw;
      }
    }

    void test_request_processor_returns_bad_request_when_executor_raises_head_error()
    {
      int sockets[2];
      if (socketpair(AF_UNIX, SOCK_STREAM, 0, sockets) < 0)
      {
        fail("socketpair failed while setting up head error executor test");
      }

      FileDescriptorGuard client_socket(sockets[0]);
      FileDescriptorGuard server_socket(sockets[1]);
      suppress_sigpipe(client_socket.get());

      const auto request_executor = std::make_shared<HeadErrorRequestExecutor>();
      const Vajra::request::RequestProcessor processor(
          Vajra::request::kDefaultMaxRequestHeadBytes,
          request_executor);
      std::thread processor_thread = start_request_processor_thread(processor, server_socket);

      try
      {
        if (!send_all(
                client_socket.get(),
                "GET /bad HTTP/1.1\r\n"
                "Host: example.test\r\n"
                "Connection: close\r\n"
                "\r\n"))
        {
          fail("failed to send request for head error executor test");
        }

        const std::string response = read_http_response(client_socket.get());
        if (response.find("HTTP/1.1 400 Bad Request\r\n") != 0)
        {
          fail("executor head error did not return a 400 response");
        }

        client_socket.close_if_open();
        processor_thread.join();
      }
      catch (...)
      {
        client_socket.close_if_open();
        if (processor_thread.joinable())
        {
          processor_thread.join();
        }
        throw;
      }
    }

    void test_request_processor_returns_internal_server_error_when_executor_response_is_invalid()
    {
      int sockets[2];
      if (socketpair(AF_UNIX, SOCK_STREAM, 0, sockets) < 0)
      {
        fail("socketpair failed while setting up invalid response executor test");
      }

      FileDescriptorGuard client_socket(sockets[0]);
      FileDescriptorGuard server_socket(sockets[1]);
      suppress_sigpipe(client_socket.get());

      const auto request_executor = std::make_shared<InvalidResponseRequestExecutor>();
      const Vajra::request::RequestProcessor processor(
          Vajra::request::kDefaultMaxRequestHeadBytes,
          request_executor);
      std::thread processor_thread = start_request_processor_thread(processor, server_socket);

      try
      {
        if (!send_all(
                client_socket.get(),
                "GET /invalid HTTP/1.1\r\n"
                "Host: example.test\r\n"
                "Connection: close\r\n"
                "\r\n"))
        {
          fail("failed to send request for invalid response executor test");
        }

        const std::string response = read_http_response(client_socket.get());
        if (response.find("HTTP/1.1 500 Internal Server Error\r\n") != 0)
        {
          fail("invalid executor response did not return a 500 response");
        }

        client_socket.close_if_open();
        processor_thread.join();
      }
      catch (...)
      {
        client_socket.close_if_open();
        if (processor_thread.joinable())
        {
          processor_thread.join();
        }
        throw;
      }
    }

    void test_request_processor_strips_executor_framing_headers_before_sending()
    {
      int sockets[2];
      if (socketpair(AF_UNIX, SOCK_STREAM, 0, sockets) < 0)
      {
        fail("socketpair failed while setting up framing header executor test");
      }

      FileDescriptorGuard client_socket(sockets[0]);
      FileDescriptorGuard server_socket(sockets[1]);
      suppress_sigpipe(client_socket.get());

      const auto request_executor = std::make_shared<FramingHeadersRequestExecutor>();
      const Vajra::request::RequestProcessor processor(
          Vajra::request::kDefaultMaxRequestHeadBytes,
          request_executor);
      std::thread processor_thread = start_request_processor_thread(processor, server_socket);

      try
      {
        if (!send_all(
                client_socket.get(),
                "GET /headers HTTP/1.1\r\n"
                "Host: example.test\r\n"
                "Connection: close\r\n"
                "\r\n"))
        {
          fail("failed to send request for framing header executor test");
        }

        const std::string response = read_http_response(client_socket.get());
        if (response.find("HTTP/1.1 200 OK\r\n") != 0)
        {
          fail("framing header executor response did not succeed");
        }

        if (response.find("Content-Length: 2\r\n") == std::string::npos)
        {
          fail("server did not restore correct content length framing");
        }

        if (response.find("Transfer-Encoding:") != std::string::npos)
        {
          fail("server leaked executor transfer-encoding framing");
        }

        if (response.find("Connection: keep-alive\r\n") != std::string::npos)
        {
          fail("server leaked executor connection framing");
        }

        client_socket.close_if_open();
        processor_thread.join();
      }
      catch (...)
      {
        client_socket.close_if_open();
        if (processor_thread.joinable())
        {
          processor_thread.join();
        }
        throw;
      }
    }

    void test_request_processor_does_not_mix_partial_internal_trace_context_with_traceparent()
    {
      char log_path[] = "/tmp/vajra-access-log-XXXXXX";
      const int log_fd = mkstemp(log_path);
      if (log_fd < 0)
      {
        fail("mkstemp failed while setting up access log correlation test");
      }
      close(log_fd);

      int sockets[2];
      if (socketpair(AF_UNIX, SOCK_STREAM, 0, sockets) < 0)
      {
        std::remove(log_path);
        fail("socketpair failed while setting up access log correlation test");
      }

      FileDescriptorGuard client_socket(sockets[0]);
      FileDescriptorGuard server_socket(sockets[1]);
      suppress_sigpipe(client_socket.get());
      Vajra::runtime::stop_runtime_logging_worker();
      Vajra::runtime::configure_runtime_logging(false, log_path, "", "json");
      Vajra::runtime::start_runtime_logging_worker();
      const auto request_executor = std::make_shared<TraceHeaderRequestExecutor>();
      const Vajra::request::RequestProcessor processor(
          Vajra::request::kDefaultMaxRequestHeadBytes,
          request_executor);
      std::thread processor_thread = start_request_processor_thread(processor, server_socket);

      try
      {
        if (!send_all(
                client_socket.get(),
                "GET /trace HTTP/1.1\r\n"
                "Host: example.test\r\n"
                "Traceparent: 00-bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb-cccccccccccccccc-01\r\n"
                "Connection: close\r\n"
                "\r\n"))
        {
          fail("failed to send request for access log correlation test");
        }

        const std::string response = read_http_response(client_socket.get());
        if (response.find("HTTP/1.1 200 OK\r\n") != 0)
        {
          fail("trace header executor response did not succeed");
        }
        if (response.find("X-Vajra-Internal-Trace-Id:") != std::string::npos)
        {
          fail("internal trace header leaked to client response");
        }

        client_socket.close_if_open();
        processor_thread.join();
        Vajra::runtime::stop_runtime_logging_worker();
        std::ifstream log_file(log_path);
        const std::string access_log((std::istreambuf_iterator<char>(log_file)), std::istreambuf_iterator<char>());
        if (access_log.find("\"trace_id\":\"aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa\"") == std::string::npos)
        {
          fail("access log did not preserve app-provided trace id");
        }
        if (access_log.find("\"span_id\"") != std::string::npos ||
            access_log.find("cccccccccccccccc") != std::string::npos)
        {
          fail("access log mixed incoming traceparent span id with app-provided trace id");
        }
        Vajra::runtime::configure_runtime_logging(false, "/dev/null", "", "text");
        std::remove(log_path);
      }
      catch (...)
      {
        client_socket.close_if_open();
        if (processor_thread.joinable())
        {
          processor_thread.join();
        }
        Vajra::runtime::stop_runtime_logging_worker();
        Vajra::runtime::configure_runtime_logging(false, "/dev/null", "", "text");
        std::remove(log_path);
        throw;
      }
    }
  }

  void run_response_tests()
  {
    test_response_serializer_serializes_status_headers_and_body();
    test_response_serializer_allows_empty_reason_phrase();
    test_response_serializer_omits_content_length_for_no_body_status();
    test_response_serializer_emits_connection_close_only_when_requested();
    test_response_serializer_preserves_header_order();
    test_response_serializer_rejects_invalid_header_name();
    test_response_serializer_rejects_invalid_header_value();
    test_response_serializer_rejects_forbidden_framing_headers();
    test_response_serializer_rejects_other_control_characters();
    test_response_serializer_rejects_invalid_status();
    test_response_serializer_rejects_bodies_for_no_body_statuses();
    test_response_writer_sends_structured_success_response();
    test_response_writer_sends_structured_error_response();
    test_response_writer_send_returns_false_on_serialization_failure();
    test_request_processor_keeps_connection_open_for_sequential_requests();
    test_request_processor_handles_pipelined_read_ahead_without_losing_the_next_request();
    test_request_processor_closes_after_parse_error_response();
    test_request_processor_keeps_connection_open_after_request_body_framing();
    test_request_body_reader_preserves_buffered_suffix_after_fixed_length_body();
    test_request_body_reader_preserves_buffered_suffix_after_chunked_body();
    test_request_body_reader_rejects_oversized_content_length_body();
    test_request_body_reader_rejects_overlong_chunk_metadata();
    test_request_body_reader_treats_chunk_metadata_at_limit_without_crlf_as_incomplete();
    test_request_body_reader_treats_transport_failures_as_incomplete_reads();
    test_request_processor_reads_fragmented_fixed_length_request_body();
    test_request_processor_decodes_chunked_request_body();
    test_request_processor_decodes_chunked_request_body_with_extensions_and_trailers();
    test_request_processor_rejects_conflicting_request_body_framing();
    test_request_processor_rejects_malformed_chunked_request_body();
    test_request_processor_rejects_oversized_request_body();
    test_request_processor_closes_quietly_when_request_body_is_incomplete();
    test_request_processor_returns_internal_server_error_when_executor_raises();
    test_request_processor_returns_bad_request_when_executor_raises_head_error();
    test_request_processor_returns_internal_server_error_when_executor_response_is_invalid();
    test_request_processor_strips_executor_framing_headers_before_sending();
    test_request_processor_does_not_mix_partial_internal_trace_context_with_traceparent();
  }
}
