// Copyright Codevedas Inc. 2025-present
//
// This source code is licensed under the MIT license found in the
// LICENSE file in the root directory of this source tree.

#include "test_support.hpp"

#include "request/request_head_parser.hpp"
#include "request/request_head_reader.hpp"
#include "response/response_writer.hpp"

#include <cerrno>
#include <chrono>
#include <cstring>
#include <stdexcept>
#include <thread>
#ifndef _WIN32
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

namespace
{
  using namespace std::chrono_literals;

}

[[noreturn]] void VajraSpecCpp::fail(const std::string &message)
{
  throw std::runtime_error(message);
}

VajraSpecCpp::SocketGuard::SocketGuard(Vajra::platform::SocketHandle fd) : fd_(fd) {}

VajraSpecCpp::SocketGuard::~SocketGuard()
{
  close_if_open();
}

Vajra::platform::SocketHandle VajraSpecCpp::SocketGuard::get() const
{
  return fd_;
}

Vajra::platform::SocketHandle VajraSpecCpp::SocketGuard::release()
{
  const auto fd = fd_;
  fd_ = Vajra::platform::kInvalidSocket;
  return fd;
}

void VajraSpecCpp::SocketGuard::close_if_open()
{
  if (Vajra::platform::socket_valid(fd_))
  {
    Vajra::platform::close_socket(fd_);
    fd_ = Vajra::platform::kInvalidSocket;
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
  Vajra::platform::ensure_socket_runtime();
  const auto fd = Vajra::platform::create_tcp_socket(AF_INET, SOCK_STREAM, 0);
  if (!Vajra::platform::socket_valid(fd))
  {
    fail("socket failed while allocating test port");
  }

  sockaddr_in addr{};
  addr.sin_family = AF_INET;
  addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  addr.sin_port = htons(0);

  if (!Vajra::platform::bind_socket(fd, reinterpret_cast<sockaddr *>(&addr), sizeof(addr)))
  {
    Vajra::platform::close_socket(fd);
    fail("bind failed while allocating test port: " + Vajra::platform::socket_error_message(Vajra::platform::socket_last_error()));
  }

  socklen_t len = sizeof(addr);
  if (!Vajra::platform::socket_name(fd, reinterpret_cast<sockaddr *>(&addr), &len))
  {
    Vajra::platform::close_socket(fd);
    fail("getsockname failed while allocating test port");
  }

  const int port = ntohs(addr.sin_port);
  Vajra::platform::close_socket(fd);
  return port;
}

Vajra::platform::SocketHandle VajraSpecCpp::connect_to_listener(int port)
{
  const auto fd = Vajra::platform::create_tcp_socket(AF_INET, SOCK_STREAM, 0);
  if (!Vajra::platform::socket_valid(fd))
  {
    fail("socket failed while connecting to test listener");
  }

  sockaddr_in addr{};
  addr.sin_family = AF_INET;
  addr.sin_port = htons(static_cast<std::uint16_t>(port));
  addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);

  if (!Vajra::platform::connect_socket(fd, reinterpret_cast<sockaddr *>(&addr), sizeof(addr)))
  {
    Vajra::platform::close_socket(fd);
    return Vajra::platform::kInvalidSocket;
  }

  return fd;
}

std::array<Vajra::platform::SocketHandle, 2> VajraSpecCpp::connected_socket_pair()
{
  Vajra::platform::ensure_socket_runtime();
#ifndef _WIN32
  std::array<Vajra::platform::SocketHandle, 2> sockets{
      Vajra::platform::kInvalidSocket,
      Vajra::platform::kInvalidSocket};
  if (socketpair(AF_UNIX, SOCK_STREAM, 0, sockets.data()) != 0)
  {
    fail("socketpair failed while creating test socket pair");
  }
  return sockets;
#else
  SocketGuard listener(Vajra::platform::create_tcp_socket(AF_INET, SOCK_STREAM, 0));
  if (!Vajra::platform::socket_valid(listener.get()))
  {
    fail("socket failed while creating loopback socket pair");
  }
  sockaddr_in address{};
  address.sin_family = AF_INET;
  address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  address.sin_port = htons(0);
  if (!Vajra::platform::bind_socket(
          listener.get(), reinterpret_cast<sockaddr *>(&address), sizeof(address)) ||
      !Vajra::platform::listen_socket(listener.get(), 1))
  {
    fail("listener setup failed while creating loopback socket pair");
  }
  socklen_t address_length = sizeof(address);
  if (!Vajra::platform::socket_name(
          listener.get(), reinterpret_cast<sockaddr *>(&address), &address_length))
  {
    fail("getsockname failed while creating loopback socket pair");
  }
  SocketGuard client(Vajra::platform::create_tcp_socket(AF_INET, SOCK_STREAM, 0));
  if (!Vajra::platform::socket_valid(client.get()) ||
      !Vajra::platform::connect_socket(
          client.get(), reinterpret_cast<sockaddr *>(&address), address_length))
  {
    fail("connect failed while creating loopback socket pair");
  }
  const auto server = Vajra::platform::accept_socket(listener.get(), nullptr, nullptr);
  if (!Vajra::platform::socket_valid(server))
  {
    fail("accept failed while creating loopback socket pair");
  }
  return {client.release(), server};
#endif
}

void VajraSpecCpp::suppress_sigpipe(Vajra::platform::SocketHandle fd)
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

bool VajraSpecCpp::send_all(Vajra::platform::SocketHandle fd, const std::string &payload)
{
  std::size_t total_sent = 0;
  while (total_sent < payload.size())
  {
    const auto bytes_sent = Vajra::platform::send_socket(fd, payload.data() + total_sent, payload.size() - total_sent);
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

bool VajraSpecCpp::complete_probe_request(Vajra::platform::SocketHandle fd)
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
  const auto bytes_read = Vajra::platform::receive_socket(fd, buffer, sizeof(buffer));
  return bytes_read > 0;
}

std::string VajraSpecCpp::read_all(Vajra::platform::SocketHandle fd)
{
  std::string response;
  char buffer[256];

  for (;;)
  {
    const auto bytes_read = Vajra::platform::receive_socket(fd, buffer, sizeof(buffer));
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

std::string VajraSpecCpp::read_http_response(Vajra::platform::SocketHandle fd)
{
  std::string response;
  char buffer[256];

  while (response.find("\r\n\r\n") == std::string::npos)
  {
    const auto bytes_read = Vajra::platform::receive_socket(fd, buffer, sizeof(buffer));
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
    const auto bytes_read = Vajra::platform::receive_socket(fd, buffer, sizeof(buffer));
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

bool VajraSpecCpp::peer_closed_within(Vajra::platform::SocketHandle fd, int timeout_ms)
{
  while (true)
  {
    if (!Vajra::platform::wait_socket(fd, Vajra::platform::WaitEvent::read, timeout_ms))
    {
      return false;
    }

    char byte = '\0';
    const auto bytes_read = Vajra::platform::peek_socket(fd, &byte, sizeof(byte));
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

      if (Vajra::platform::socket_error_disconnected(errno))
      {
        return true;
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
    const auto fd = connect_to_listener(port);
    if (Vajra::platform::socket_valid(fd))
    {
      const bool completed_request = complete_probe_request(fd);
      Vajra::platform::close_socket(fd);
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
  const auto fd = Vajra::platform::create_tcp_socket(AF_INET, SOCK_STREAM, 0);
  if (!Vajra::platform::socket_valid(fd))
  {
    fail("socket failed while checking port rebind");
  }

  int opt = 1;
  if (!Vajra::platform::set_socket_option(fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt)))
  {
    Vajra::platform::close_socket(fd);
    fail("setsockopt failed while checking port rebind");
  }

  sockaddr_in addr{};
  addr.sin_family = AF_INET;
  addr.sin_addr.s_addr = htonl(INADDR_ANY);
  addr.sin_port = htons(static_cast<std::uint16_t>(port));

  if (!Vajra::platform::bind_socket(fd, reinterpret_cast<sockaddr *>(&addr), sizeof(addr)))
  {
    Vajra::platform::close_socket(fd);
    fail("listener port was not released after stop");
  }

  Vajra::platform::close_socket(fd);
}

VajraSpecCpp::ReaderOutcome VajraSpecCpp::read_request_head_from_chunks(
    const std::vector<std::string> &chunks,
    std::size_t max_request_head_bytes)
{
  const auto sockets = connected_socket_pair();

  SocketGuard reader_socket(sockets[0]);
  SocketGuard writer_socket(sockets[1]);
  suppress_sigpipe(writer_socket.get());
  Vajra::request::HeadReader reader(max_request_head_bytes);
  ReaderOutcome outcome{{false, false, "", ""}, nullptr};

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

}

std::string VajraSpecCpp::send_response_through_socket(const Vajra::response::Response &response)
{
  const auto sockets = connected_socket_pair();

  SocketGuard reader_socket(sockets[0]);
  SocketGuard writer_socket(sockets[1]);

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
