// Copyright Codevedas Inc. 2025-present
//
// This source code is licensed under the MIT license found in the
// LICENSE file in the root directory of this source tree.

#include "request/request_head_size_validator.hpp"
#include "request/request_processor.hpp"
#include "response/response_serializer.hpp"
#include "response/response_writer.hpp"
#include "test_suites.hpp"
#include "test_support.hpp"

#include <string>
#include <sys/socket.h>
#include <thread>
#include <unistd.h>

namespace VajraSpecCpp
{
  namespace
  {
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
      return std::thread([&processor, owned_fd]() { processor.handle(owned_fd); });
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

    void test_request_processor_closes_after_request_body_framing()
    {
      int sockets[2];
      if (socketpair(AF_UNIX, SOCK_STREAM, 0, sockets) < 0)
      {
        fail("socketpair failed while setting up request body framing test");
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

        if (response.find("Connection: close\r\n") == std::string::npos)
        {
          fail("request with unread body framing did not force connection close");
        }

        if (!peer_closed_within(client_socket.get(), 500))
        {
          fail("request processor left the connection open after unread body framing");
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
    test_request_processor_closes_after_parse_error_response();
    test_request_processor_closes_after_request_body_framing();
  }
}
