// Copyright Codevedas Inc. 2025-present
//
// This source code is licensed under the MIT license found in the
// LICENSE file in the root directory of this source tree.

#include "server.hpp"
#include "vajra.hpp"

#include <arpa/inet.h>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <iostream>
#include <netinet/in.h>
#include <stdexcept>
#include <string>
#include <sys/socket.h>
#include <thread>
#include <unistd.h>

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

  [[noreturn]] void fail(const std::string &message)
  {
    throw std::runtime_error(message);
  }

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

  bool complete_probe_request(int fd)
  {
    const char *request =
        "GET / HTTP/1.1\r\n"
        "Host: localhost\r\n"
        "Connection: close\r\n"
        "\r\n";

    const ssize_t bytes_sent = send(fd, request, std::strlen(request), 0);
    if (bytes_sent < 0)
    {
      return false;
    }

    char buffer[4096];
    const ssize_t bytes_read = recv(fd, buffer, sizeof(buffer), 0);
    return bytes_read > 0;
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
    Vajra::request::RequestHeadParser parser;
    const Vajra::request::ParsedRequest request = parser.parse(
        "GET /projects?filter=active HTTP/1.1\r\n"
        "Host: example.test\r\n"
        "User-Agent: vajra-test\r\n"
        "X-Trace-Id: abc123\r\n"
        "\r\n");

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

  void test_parse_request_head_rejects_malformed_request_line()
  {
    expect_parse_error(
        "GET /only-two-parts\r\n"
        "Host: example.test\r\n"
        "\r\n",
        Vajra::request::HeadFailureKind::bad_request,
        "invalid request line");
  }

  void test_parse_request_head_rejects_invalid_header_line()
  {
    expect_parse_error(
        "GET / HTTP/1.1\r\n"
        "Host example.test\r\n"
        "\r\n",
        Vajra::request::HeadFailureKind::bad_request,
        "invalid header line");
  }

  void test_parse_request_head_rejects_invalid_http_version()
  {
    expect_parse_error(
        "GET / HTTP/2.0\r\n"
        "Host: example.test\r\n"
        "\r\n",
        Vajra::request::HeadFailureKind::bad_request,
        "invalid HTTP version");
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
}

int main()
{
  try
  {
    test_start_and_stop_release_listener();
    test_stop_before_start_exits_cleanly();
    test_parse_request_head_parses_request_line_and_headers();
    test_parse_request_head_rejects_malformed_request_line();
    test_parse_request_head_rejects_invalid_header_line();
    test_parse_request_head_rejects_invalid_http_version();
    test_validate_request_head_size_rejects_oversized_request_head();
  }
  catch (const std::exception &error)
  {
    std::cerr << error.what() << '\n';
    return EXIT_FAILURE;
  }

  return EXIT_SUCCESS;
}
