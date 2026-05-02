// Copyright Codevedas Inc. 2025-present
//
// This source code is licensed under the MIT license found in the
// LICENSE file in the root directory of this source tree.

#include "test_support.hpp"

#include "request/request_head_parser.hpp"
#include "request/request_head_reader.hpp"
#include "response/response_writer.hpp"

#include <arpa/inet.h>
#include <cerrno>
#include <chrono>
#include <cstring>
#include <netinet/in.h>
#include <poll.h>
#include <stdexcept>
#include <sys/socket.h>
#include <thread>
#include <unistd.h>

namespace
{
  using namespace std::chrono_literals;

#ifdef MSG_NOSIGNAL
  constexpr int kSendFlags = MSG_NOSIGNAL;
#else
  constexpr int kSendFlags = 0;
#endif
}

[[noreturn]] void VajraSpecCpp::fail(const std::string &message)
{
  throw std::runtime_error(message);
}

VajraSpecCpp::FileDescriptorGuard::FileDescriptorGuard(int fd) : fd_(fd) {}

VajraSpecCpp::FileDescriptorGuard::~FileDescriptorGuard()
{
  close_if_open();
}

int VajraSpecCpp::FileDescriptorGuard::get() const
{
  return fd_;
}

void VajraSpecCpp::FileDescriptorGuard::close_if_open()
{
  if (fd_ >= 0)
  {
    close(fd_);
    fd_ = -1;
  }
}

bool VajraSpecCpp::bind_conflict(const std::exception_ptr &error)
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

int VajraSpecCpp::available_port()
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

int VajraSpecCpp::connect_to_listener(int port)
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

void VajraSpecCpp::suppress_sigpipe(int fd)
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

bool VajraSpecCpp::send_all(int fd, const std::string &payload)
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

bool VajraSpecCpp::complete_probe_request(int fd)
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

std::string VajraSpecCpp::read_all(int fd)
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

std::size_t VajraSpecCpp::parse_content_length(const std::string &response)
{
  const std::string prefix = "Content-Length: ";
  const std::size_t header_start = response.find(prefix);
  if (header_start == std::string::npos)
  {
    return 0;
  }

  const std::size_t value_start = header_start + prefix.size();
  const std::size_t value_end = response.find("\r\n", value_start);
  if (value_end == std::string::npos)
  {
    fail("response content length header was not terminated");
  }

  const std::string value = response.substr(value_start, value_end - value_start);
  if (value.empty())
  {
    fail("response content length header was empty");
  }

  std::size_t content_length = 0;
  for (const char character : value)
  {
    if (character < '0' || character > '9')
    {
      fail("response content length header was not numeric");
    }

    content_length = content_length * 10 + static_cast<std::size_t>(character - '0');
  }

  return content_length;
}

std::string VajraSpecCpp::read_http_response(int fd)
{
  std::string response;
  char buffer[256];

  while (response.find("\r\n\r\n") == std::string::npos)
  {
    const ssize_t bytes_read = recv(fd, buffer, sizeof(buffer), 0);
    if (bytes_read < 0)
    {
      if (errno == EINTR)
      {
        continue;
      }

      fail("recv failed while reading response headers");
    }

    if (bytes_read == 0)
    {
      fail("connection closed before response headers completed");
    }

    response.append(buffer, static_cast<std::size_t>(bytes_read));
  }

  const std::size_t header_boundary = response.find("\r\n\r\n");
  const std::size_t total_size = header_boundary + 4 + parse_content_length(response);

  while (response.size() < total_size)
  {
    const ssize_t bytes_read = recv(fd, buffer, sizeof(buffer), 0);
    if (bytes_read < 0)
    {
      if (errno == EINTR)
      {
        continue;
      }

      fail("recv failed while reading response body");
    }

    if (bytes_read == 0)
    {
      fail("connection closed before response body completed");
    }

    response.append(buffer, static_cast<std::size_t>(bytes_read));
  }

  return response.substr(0, total_size);
}

bool VajraSpecCpp::peer_closed_within(int fd, int timeout_ms)
{
  pollfd descriptor{fd, POLLIN | POLLHUP | POLLERR, 0};

  while (true)
  {
    const int poll_result = poll(&descriptor, 1, timeout_ms);
    if (poll_result < 0)
    {
      if (errno == EINTR)
      {
        continue;
      }

      fail("poll failed while checking socket closure");
    }

    if (poll_result == 0)
    {
      return false;
    }

    if ((descriptor.revents & (POLLHUP | POLLERR)) != 0)
    {
      return true;
    }

    char byte = '\0';
    const ssize_t bytes_read = recv(fd, &byte, sizeof(byte), MSG_PEEK);
    if (bytes_read < 0)
    {
      if (errno == EINTR)
      {
        continue;
      }

      if (errno == EAGAIN || errno == EWOULDBLOCK)
      {
        return false;
      }

      fail("recv(MSG_PEEK) failed while checking socket closure");
    }

    return bytes_read == 0;
  }
}

void VajraSpecCpp::wait_until_listening(int port)
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

void VajraSpecCpp::assert_can_rebind(int port)
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

VajraSpecCpp::ReaderOutcome VajraSpecCpp::read_request_head_from_chunks(
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

void VajraSpecCpp::expect_parse_success(
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

void VajraSpecCpp::expect_parse_error(
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

void VajraSpecCpp::expect_reader_error(
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

std::string VajraSpecCpp::send_response_through_socket(const Vajra::response::Response &response)
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
