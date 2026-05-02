// Copyright Codevedas Inc. 2025-present
//
// This source code is licensed under the MIT license found in the
// LICENSE file in the root directory of this source tree.

#include "request/request_head_error.hpp"
#include "request/request_head_parser.hpp"
#include "request/request_head_reader.hpp"
#include "request/request_head_size_validator.hpp"
#include "response/response_serializer.hpp"
#include "response/response_writer.hpp"
#include "server.hpp"
#include "vajra.hpp"

#include <arpa/inet.h>
#include <cerrno>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <iostream>
#include <netinet/in.h>
#include <random>
#include <stdexcept>
#include <string>
#include <sys/socket.h>
#include <thread>
#include <unistd.h>
#include <vector>

namespace VajraNative
{
  bool shutdown_requested()
  {
    return false;
  }
}

namespace
{
  using namespace std::chrono_literals;

#ifdef MSG_NOSIGNAL
  constexpr int kSendFlags = MSG_NOSIGNAL;
#else
  constexpr int kSendFlags = 0;
#endif

  [[noreturn]] void fail(const std::string &message)
  {
    throw std::runtime_error(message);
  }

  class FileDescriptorGuard
  {
  public:
    explicit FileDescriptorGuard(int fd) : fd_(fd) {}
    FileDescriptorGuard(const FileDescriptorGuard &) = delete;
    FileDescriptorGuard &operator=(const FileDescriptorGuard &) = delete;

    ~FileDescriptorGuard()
    {
      close_if_open();
    }

    int get() const
    {
      return fd_;
    }

    void close_if_open()
    {
      if (fd_ >= 0)
      {
        close(fd_);
        fd_ = -1;
      }
    }

  private:
    int fd_;
  };

  struct ReaderOutcome
  {
    Vajra::request::HeadReadResult result;
    std::exception_ptr error;
  };

  bool bind_conflict(const std::exception_ptr &error)
  {
    if (!error)
    {
      return false;
    }

    try
    {
      std::rethrow_exception(error);
    }
    catch (const std::runtime_error &runtime_error)
    {
      return std::string(runtime_error.what()).find("Address already in use") != std::string::npos;
    }
    catch (...)
    {
      return false;
    }
  }

  int available_port()
  {
    const int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0)
    {
      fail("socket failed while allocating test port");
    }

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = htons(0);

    if (bind(fd, reinterpret_cast<sockaddr *>(&addr), sizeof(addr)) < 0)
    {
      close(fd);
      fail("bind failed while allocating test port");
    }

    socklen_t len = sizeof(addr);
    if (getsockname(fd, reinterpret_cast<sockaddr *>(&addr), &len) < 0)
    {
      close(fd);
      fail("getsockname failed while allocating test port");
    }

    const int port = ntohs(addr.sin_port);
    close(fd);
    return port;
  }

  int connect_to_listener(int port)
  {
    const int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0)
    {
      fail("socket failed while connecting to test listener");
    }

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);

    if (connect(fd, reinterpret_cast<sockaddr *>(&addr), sizeof(addr)) < 0)
    {
      close(fd);
      return -1;
    }

    return fd;
  }

  void suppress_sigpipe(int fd)
  {
#ifdef SO_NOSIGPIPE
    int opt = 1;
    if (setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &opt, sizeof(opt)) < 0)
    {
      fail("setsockopt(SO_NOSIGPIPE) failed while configuring test socket");
    }
#else
    (void)fd;
#endif
  }

  bool send_all(int fd, const std::string &payload)
  {
    std::size_t total_sent = 0;
    while (total_sent < payload.size())
    {
      const ssize_t bytes_sent = send(fd, payload.data() + total_sent, payload.size() - total_sent, kSendFlags);
      if (bytes_sent < 0)
      {
        if (errno == EINTR)
        {
          continue;
        }

        if (errno == EPIPE || errno == ECONNRESET)
        {
          return false;
        }

        fail("send failed while writing test payload");
      }

      if (bytes_sent == 0)
      {
        return false;
      }

      total_sent += static_cast<std::size_t>(bytes_sent);
    }

    return true;
  }

  bool complete_probe_request(int fd)
  {
    const std::string request =
        "GET / HTTP/1.1\r\n"
        "Host: localhost\r\n"
        "Connection: close\r\n"
        "\r\n";

    if (!send_all(fd, request))
    {
      return false;
    }

    char buffer[4096];
    const ssize_t bytes_read = recv(fd, buffer, sizeof(buffer), 0);
    return bytes_read > 0;
  }

  std::string read_all(int fd)
  {
    std::string response;
    char buffer[256];

    for (;;)
    {
      const ssize_t bytes_read = recv(fd, buffer, sizeof(buffer), 0);
      if (bytes_read < 0)
      {
        if (errno == EINTR)
        {
          continue;
        }

        fail("recv failed while reading response bytes");
      }

      if (bytes_read == 0)
      {
        break;
      }

      response.append(buffer, static_cast<std::size_t>(bytes_read));
    }

    return response;
  }

  void wait_until_listening(int port)
  {
    for (int attempt = 0; attempt < 200; ++attempt)
    {
      const int fd = connect_to_listener(port);
      if (fd >= 0)
      {
        const bool completed_request = complete_probe_request(fd);
        close(fd);
        if (completed_request)
        {
          return;
        }
      }

      std::this_thread::sleep_for(10ms);
    }

    fail("server did not begin listening in time");
  }

  void assert_can_rebind(int port)
  {
    const int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0)
    {
      fail("socket failed while checking port rebind");
    }

    int opt = 1;
    if (setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt)) < 0)
    {
      close(fd);
      fail("setsockopt failed while checking port rebind");
    }

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port = htons(port);

    if (bind(fd, reinterpret_cast<sockaddr *>(&addr), sizeof(addr)) < 0)
    {
      close(fd);
      fail("listener port was not released after stop");
    }

    close(fd);
  }

  ReaderOutcome read_request_head_from_chunks(
      const std::vector<std::string> &chunks,
      std::size_t max_request_head_bytes)
  {
    int sockets[2];
    if (socketpair(AF_UNIX, SOCK_STREAM, 0, sockets) < 0)
    {
      fail("socketpair failed while setting up reader test");
    }

    FileDescriptorGuard reader_socket(sockets[0]);
    FileDescriptorGuard writer_socket(sockets[1]);
    suppress_sigpipe(writer_socket.get());
    Vajra::request::HeadReader reader(max_request_head_bytes);
    ReaderOutcome outcome{{false, ""}, nullptr};

    std::thread reader_thread([&]() {
      try
      {
        outcome.result = reader.read(reader_socket.get());
      }
      catch (...)
      {
        outcome.error = std::current_exception();
      }

      reader_socket.close_if_open();
    });

    try
    {
      for (const std::string &chunk : chunks)
      {
        if (!send_all(writer_socket.get(), chunk))
        {
          break;
        }
        std::this_thread::sleep_for(5ms);
      }

      writer_socket.close_if_open();
      reader_thread.join();
    }
    catch (...)
    {
      writer_socket.close_if_open();
      if (reader_thread.joinable())
      {
        reader_thread.join();
      }
      throw;
    }

    return outcome;
  }

  void expect_parse_success(
      const std::string &request_head,
      const std::string &expected_method,
      const std::string &expected_target,
      const std::string &expected_version,
      std::size_t expected_header_count)
  {
    Vajra::request::RequestHeadParser parser;
    const Vajra::request::ParsedRequest request = parser.parse(request_head);

    if (request.request_line.method != expected_method)
    {
      fail("request method was not parsed correctly");
    }

    if (request.request_line.target != expected_target)
    {
      fail("request target was not parsed correctly");
    }

    if (request.request_line.version != expected_version)
    {
      fail("request version was not parsed correctly");
    }

    if (request.headers.size() != expected_header_count)
    {
      fail("request headers were not parsed correctly");
    }
  }

  void expect_parse_error(
      const std::string &request_head,
      Vajra::request::HeadFailureKind expected_kind,
      const std::string &expected_message)
  {
    try
    {
      Vajra::request::RequestHeadParser parser;
      (void)parser.parse(request_head);
    }
    catch (const Vajra::request::HeadError &error)
    {
      if (error.kind() != expected_kind)
      {
        fail("unexpected parse error kind");
      }

      if (std::string(error.what()).find(expected_message) != std::string::npos)
      {
        return;
      }

      fail(
          "unexpected parse error. expected message containing \"" + expected_message + "\", got: " +
          error.what());
    }

    fail("request head was not rejected");
  }

  void expect_reader_error(
      const std::vector<std::string> &chunks,
      std::size_t max_request_head_bytes,
      Vajra::request::HeadFailureKind expected_kind,
      const std::string &expected_message)
  {
    const ReaderOutcome outcome = read_request_head_from_chunks(chunks, max_request_head_bytes);
    if (!outcome.error)
    {
      fail("reader was expected to reject the request head");
    }

    try
    {
      std::rethrow_exception(outcome.error);
    }
    catch (const Vajra::request::HeadError &error)
    {
      if (error.kind() != expected_kind)
      {
        fail("reader used the wrong failure kind");
      }

      if (std::string(error.what()).find(expected_message) == std::string::npos)
      {
        fail("reader used the wrong failure message");
      }

      return;
    }

    fail("reader raised the wrong exception type");
  }

  std::string send_response_through_socket(const Vajra::response::Response &response)
  {
    int sockets[2];
    if (socketpair(AF_UNIX, SOCK_STREAM, 0, sockets) < 0)
    {
      fail("socketpair failed while setting up response writer test");
    }

    FileDescriptorGuard reader_socket(sockets[0]);
    FileDescriptorGuard writer_socket(sockets[1]);

    {
      Vajra::response::ResponseWriter writer;
      if (!writer.send(writer_socket.get(), response))
      {
        fail("response writer failed to send a valid response");
      }
    }

    writer_socket.close_if_open();
    return read_all(reader_socket.get());
  }

  void test_start_and_stop_release_listener()
  {
    for (int attempt = 0; attempt < 10; ++attempt)
    {
      const int port = available_port();
      Vajra::Server server(port, Vajra::request::kDefaultMaxRequestHeadBytes);
      std::exception_ptr server_error;

      std::thread server_thread([&]() {
        try
        {
          server.start();
        }
        catch (...)
        {
          server_error = std::current_exception();
        }
      });

      try
      {
        wait_until_listening(port);
        server.stop();
        server_thread.join();

        if (server_error)
        {
          std::rethrow_exception(server_error);
        }

        assert_can_rebind(port);
        return;
      }
      catch (...)
      {
        if (server_thread.joinable())
        {
          server.stop();
          server_thread.join();
        }

        if (bind_conflict(server_error) && attempt < 9)
        {
          continue;
        }

        throw;
      }
    }

    fail("server could not obtain a reusable listener port after retries");
  }

  void test_stop_before_start_exits_cleanly()
  {
    const int port = available_port();
    Vajra::Server server(port, Vajra::request::kDefaultMaxRequestHeadBytes);
    server.stop();
    server.start();
    assert_can_rebind(port);
  }

  void test_parse_request_head_parses_request_line_and_headers()
  {
    const std::string request_head =
        "GET /projects?filter=active HTTP/1.1\r\n"
        "Host: example.test\r\n"
        "User-Agent: vajra-test\r\n"
        "X-Trace-Id: abc123\r\n"
        "\r\n";

    Vajra::request::RequestHeadParser parser;
    const Vajra::request::ParsedRequest request = parser.parse(request_head);

    if (request.request_line.method != "GET")
    {
      fail("request method was not parsed correctly");
    }

    if (request.request_line.target != "/projects?filter=active")
    {
      fail("request target was not parsed correctly");
    }

    if (request.request_line.version != "HTTP/1.1")
    {
      fail("request version was not parsed correctly");
    }

    if (request.headers.size() != 3)
    {
      fail("request headers were not parsed correctly");
    }

    if (request.headers[0].name != "Host" || request.headers[0].value != "example.test")
    {
      fail("first request header was not parsed correctly");
    }

    if (request.headers[2].name != "X-Trace-Id" || request.headers[2].value != "abc123")
    {
      fail("later request headers were not parsed correctly");
    }
  }

  void test_parse_request_head_preserves_header_order_and_empty_values()
  {
    Vajra::request::RequestHeadParser parser;
    const Vajra::request::ParsedRequest request = parser.parse(
        "POST /submit HTTP/1.1\r\n"
        "Host: example.test\r\n"
        "X-Empty:\r\n"
        "X-Trimmed:   kept\r\n"
        "\r\n");

    if (request.headers.size() != 3)
    {
      fail("header count did not match expected value");
    }

    if (request.headers[1].name != "X-Empty" || request.headers[1].value != "")
    {
      fail("empty header value was not preserved");
    }

    if (request.headers[2].name != "X-Trimmed" || request.headers[2].value != "kept")
    {
      fail("header value trimming or ordering regressed");
    }
  }

  void test_parse_request_head_rejects_malformed_request_line_variants()
  {
    const std::vector<std::string> invalid_request_heads = {
        "GET /only-two-parts\r\nHost: example.test\r\n\r\n",
        " /missing-method HTTP/1.1\r\nHost: example.test\r\n\r\n",
        "GET  HTTP/1.1\r\nHost: example.test\r\n\r\n",
        "GET / HTTP/1.1 EXTRA\r\nHost: example.test\r\n\r\n"};

    for (const std::string &request_head : invalid_request_heads)
    {
      expect_parse_error(request_head, Vajra::request::HeadFailureKind::bad_request, "invalid request line");
    }
  }

  void test_parse_request_head_rejects_invalid_header_variants()
  {
    const std::vector<std::string> invalid_request_heads = {
        "GET / HTTP/1.1\r\nHost example.test\r\n\r\n",
        "GET / HTTP/1.1\r\n: example.test\r\n\r\n",
        "GET / HTTP/1.1\r\nBad Header: value\r\n\r\n"};

    expect_parse_error(
        invalid_request_heads[0],
        Vajra::request::HeadFailureKind::bad_request,
        "invalid header line");
    expect_parse_error(
        invalid_request_heads[1],
        Vajra::request::HeadFailureKind::bad_request,
        "invalid header line");
    expect_parse_error(
        invalid_request_heads[2],
        Vajra::request::HeadFailureKind::bad_request,
        "invalid header name");
  }

  void test_parse_request_head_rejects_invalid_http_version_variants()
  {
    const std::vector<std::string> invalid_request_heads = {
        "GET / HTTP/2.0\r\nHost: example.test\r\n\r\n",
        "GET / HTTP/1.0\r\nHost: example.test\r\n\r\n",
        "GET / http/1.1\r\nHost: example.test\r\n\r\n"};

    for (const std::string &request_head : invalid_request_heads)
    {
      expect_parse_error(request_head, Vajra::request::HeadFailureKind::bad_request, "invalid HTTP version");
    }
  }

  void test_validate_request_head_size_rejects_oversized_request_head()
  {
    try
    {
      Vajra::request::RequestHeadSizeValidator validator(Vajra::request::kDefaultMaxRequestHeadBytes);
      validator.validate(Vajra::request::kDefaultMaxRequestHeadBytes + 1);
    }
    catch (const Vajra::request::HeadError &error)
    {
      if (error.kind() != Vajra::request::HeadFailureKind::header_too_large)
      {
        fail("oversized request head used the wrong failure kind");
      }

      if (std::string(error.what()).find("request head exceeds maximum size") == std::string::npos)
      {
        fail("oversized request head used the wrong failure message");
      }

      return;
    }

    fail("oversized request head was not rejected");
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
        "done"};

    const std::string serialized = serializer.serialize(response);
    const std::string expected =
        "HTTP/1.1 201 Created\r\n"
        "Content-Type: text/plain\r\n"
        "X-Trace-Id: trace-123\r\n"
        "Content-Length: 4\r\n"
        "Connection: close\r\n"
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

    if (serialized.find("Connection: close\r\n\r\n") == std::string::npos)
    {
      fail("no-body statuses did not terminate headers correctly");
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
    const std::string response = send_response_through_socket(writer.success_response());

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

    if (response.find("\r\n\r\nOK") == std::string::npos)
    {
      fail("success response body did not match the serialized payload");
    }
  }

  void test_response_writer_sends_structured_error_response()
  {
    Vajra::response::ResponseWriter writer;
    const std::string response =
        send_response_through_socket(writer.request_head_failure_response(Vajra::request::HeadFailureKind::header_too_large));

    if (response.find("HTTP/1.1 431 Request Header Fields Too Large\r\n") != 0)
    {
      fail("error response did not start with the expected status line");
    }

    if (response.find("Content-Length: 31\r\n") == std::string::npos)
    {
      fail("error response used the wrong content length");
    }

    if (response.find("\r\n\r\nRequest Header Fields Too Large") == std::string::npos)
    {
      fail("error response body did not match the serialized payload");
    }
  }

  void test_head_reader_accepts_fragmented_request_head()
  {
    const std::vector<std::string> chunks = {
        "GET /fragmented HTTP/1.1\r\nHost: example.test\r\nConnection: close\r",
        "\n",
        "\r",
        "\n"};

    const std::string expected_request_head =
        "GET /fragmented HTTP/1.1\r\nHost: example.test\r\nConnection: close\r\n\r\n";
    const ReaderOutcome outcome = read_request_head_from_chunks(chunks, expected_request_head.size());

    if (outcome.error)
    {
      std::rethrow_exception(outcome.error);
    }

    if (!outcome.result.complete)
    {
      fail("fragmented request head was not marked complete");
    }

    if (outcome.result.request_head != expected_request_head)
    {
      fail("fragmented request head bytes were not preserved");
    }
  }

  void test_head_reader_returns_incomplete_on_peer_close_before_boundary()
  {
    const ReaderOutcome outcome = read_request_head_from_chunks(
        {"GET /partial HTTP/1.1\r\nHost: example.test\r\nConnection: close\r\n"},
        Vajra::request::kDefaultMaxRequestHeadBytes);

    if (outcome.error)
    {
      std::rethrow_exception(outcome.error);
    }

    if (outcome.result.complete)
    {
      fail("partial request head was incorrectly marked complete");
    }

    if (outcome.result.request_head.empty())
    {
      fail("partial request head bytes were not captured");
    }
  }

  void test_head_reader_accepts_exact_limit_request_head()
  {
    const std::string request_head =
        "GET /exact-limit HTTP/1.1\r\nHost: example.test\r\nX-Fill: 123456\r\n\r\n";
    const ReaderOutcome outcome = read_request_head_from_chunks({request_head}, request_head.size());

    if (outcome.error)
    {
      std::rethrow_exception(outcome.error);
    }

    if (!outcome.result.complete)
    {
      fail("exact-limit request head was not accepted");
    }

    if (outcome.result.request_head.size() != request_head.size())
    {
      fail("exact-limit request head size changed unexpectedly");
    }
  }

  void test_head_reader_rejects_limit_plus_one_request_head()
  {
    const std::string request_head =
        "GET /limit-plus-one HTTP/1.1\r\nHost: example.test\r\nX-Fill: 1234567\r\n\r\n";

    expect_reader_error(
        {request_head},
        request_head.size() - 1,
        Vajra::request::HeadFailureKind::header_too_large,
        "request head exceeds maximum size");
  }

  void test_property_generated_valid_request_heads_parse()
  {
    std::mt19937 generator(29);

    for (int index = 0; index < 50; ++index)
    {
      const int path_suffix = static_cast<int>(generator() % 10000);
      const int trace_suffix = static_cast<int>(generator() % 100000);
      const std::string method = (index % 2 == 0) ? "GET" : "POST";
      const std::string target = "/generated/" + std::to_string(path_suffix) + "?case=" + std::to_string(index);
      const std::string request_head =
          method + " " + target + " HTTP/1.1\r\n" +
          "Host: generated.test\r\n" +
          "X-Trace-Id: trace-" + std::to_string(trace_suffix) + "\r\n" +
          "X-Optional:" + (index % 3 == 0 ? "" : " value") + "\r\n" +
          "\r\n";

      expect_parse_success(request_head, method, target, "HTTP/1.1", 3);
    }
  }

  void test_property_generated_invalid_request_lines_reject()
  {
    std::mt19937 generator(291);

    for (int index = 0; index < 50; ++index)
    {
      const int suffix = static_cast<int>(generator() % 10000);
      const std::string malformed_request_line =
          (index % 2 == 0)
              ? "GET  HTTP/1.1"
              : "GET /generated/" + std::to_string(suffix) + " HTTP/1.1 EXTRA";

      expect_parse_error(
          malformed_request_line + "\r\nHost: generated.test\r\n\r\n",
          Vajra::request::HeadFailureKind::bad_request,
          "invalid request line");
    }
  }

  void test_property_generated_invalid_headers_reject()
  {
    std::mt19937 generator(292);

    for (int index = 0; index < 50; ++index)
    {
      const int suffix = static_cast<int>(generator() % 10000);
      const std::string invalid_header =
          (index % 2 == 0)
              ? "Bad Header " + std::to_string(suffix) + ": value"
              : "MissingColon " + std::to_string(suffix);

      expect_parse_error(
          "GET /generated HTTP/1.1\r\n" + invalid_header + "\r\n\r\n",
          Vajra::request::HeadFailureKind::bad_request,
          index % 2 == 0 ? "invalid header name" : "invalid header line");
    }
  }

  void test_property_generated_partial_request_heads_never_complete()
  {
    const std::string complete_request_head =
        "GET /partial-generated HTTP/1.1\r\nHost: example.test\r\nConnection: close\r\n\r\n";

    for (std::size_t prefix_length = 1; prefix_length + 1 < complete_request_head.size(); prefix_length += 7)
    {
      const ReaderOutcome outcome = read_request_head_from_chunks(
          {complete_request_head.substr(0, prefix_length)},
          Vajra::request::kDefaultMaxRequestHeadBytes);

      if (outcome.error)
      {
        std::rethrow_exception(outcome.error);
      }

      if (outcome.result.complete)
      {
        fail("partial request head was incorrectly marked complete in generated coverage");
      }
    }
  }

  void test_property_generated_near_boundary_size_classification()
  {
    for (std::size_t fill_length = 0; fill_length < 32; ++fill_length)
    {
      const std::string base_request_head =
          "GET /size-check HTTP/1.1\r\nHost: example.test\r\nX-Fill: " +
          std::string(fill_length, 'a') + "\r\n\r\n";
      const std::size_t exact_limit = base_request_head.size();

      const ReaderOutcome exact_outcome = read_request_head_from_chunks({base_request_head}, exact_limit);
      if (exact_outcome.error)
      {
        std::rethrow_exception(exact_outcome.error);
      }

      if (!exact_outcome.result.complete)
      {
        fail("near-boundary exact request head was not accepted");
      }

      expect_reader_error(
          {base_request_head},
          exact_limit - 1,
          Vajra::request::HeadFailureKind::header_too_large,
          "request head exceeds maximum size");
    }
  }
}

int main()
{
  try
  {
    test_start_and_stop_release_listener();
    test_stop_before_start_exits_cleanly();
    test_parse_request_head_parses_request_line_and_headers();
    test_parse_request_head_preserves_header_order_and_empty_values();
    test_parse_request_head_rejects_malformed_request_line_variants();
    test_parse_request_head_rejects_invalid_header_variants();
    test_parse_request_head_rejects_invalid_http_version_variants();
    test_validate_request_head_size_rejects_oversized_request_head();
    test_response_serializer_serializes_status_headers_and_body();
    test_response_serializer_allows_empty_reason_phrase();
    test_response_serializer_omits_content_length_for_no_body_status();
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
    test_head_reader_accepts_fragmented_request_head();
    test_head_reader_returns_incomplete_on_peer_close_before_boundary();
    test_head_reader_accepts_exact_limit_request_head();
    test_head_reader_rejects_limit_plus_one_request_head();
    test_property_generated_valid_request_heads_parse();
    test_property_generated_invalid_request_lines_reject();
    test_property_generated_invalid_headers_reject();
    test_property_generated_partial_request_heads_never_complete();
    test_property_generated_near_boundary_size_classification();
  }
  catch (const std::exception &error)
  {
    std::cerr << error.what() << '\n';
    return EXIT_FAILURE;
  }

  return EXIT_SUCCESS;
}
