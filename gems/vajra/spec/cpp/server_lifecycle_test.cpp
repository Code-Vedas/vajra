// Copyright Codevedas Inc. 2025-present
//
// This source code is licensed under the MIT license found in the
// LICENSE file in the root directory of this source tree.

#include "request/request_head_size_validator.hpp"
#include "server.hpp"
#include "test_suites.hpp"
#include "test_support.hpp"

#include <exception>
#include <sstream>
#include <thread>

namespace VajraSpecCpp
{
  namespace
  {
    void expect_stopped_snapshot(
        const Vajra::Server &server,
        Vajra::lifecycle::StopReason expected_reason);

    template <typename Scenario>
    void run_stop_interrupt_test(const Scenario &scenario, const std::string &retry_failure_message)
    {
      for (int attempt = 0; attempt < 10; ++attempt)
      {
        const int port = available_port();
        Vajra::Server server(port, "0.0.0.0", Vajra::request::kDefaultMaxRequestHeadBytes);
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
          scenario(port, server);
          if (server_thread.joinable())
          {
            server_thread.join();
          }
          if (server_error)
          {
            std::rethrow_exception(server_error);
          }
          expect_stopped_snapshot(server, Vajra::lifecycle::StopReason::programmatic_stop);
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

      fail(retry_failure_message);
    }

    void expect_stopped_snapshot(
        const Vajra::Server &server,
        Vajra::lifecycle::StopReason expected_reason)
    {
      const Vajra::lifecycle::Snapshot snapshot = server.lifecycle_snapshot();
      if (snapshot.state != Vajra::lifecycle::State::stopped ||
          snapshot.last_stop_reason != expected_reason ||
          snapshot.listener_owned)
      {
        std::ostringstream message;
        message << "server lifecycle snapshot did not reach the expected stopped state"
                << " state=" << static_cast<int>(snapshot.state)
                << " stop_reason=" << static_cast<int>(snapshot.last_stop_reason)
                << " listener_owned=" << snapshot.listener_owned
                << " port=" << snapshot.port
                << " listener_fd=" << snapshot.listener_fd;
        fail(message.str());
      }
    }

    void test_start_and_stop_release_listener()
    {
      for (int attempt = 0; attempt < 10; ++attempt)
      {
        const int port = available_port();
        Vajra::Server server(port, "0.0.0.0", Vajra::request::kDefaultMaxRequestHeadBytes);
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
          expect_stopped_snapshot(server, Vajra::lifecycle::StopReason::programmatic_stop);
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
      Vajra::Server server(port, "0.0.0.0", Vajra::request::kDefaultMaxRequestHeadBytes);
      server.stop();
      server.start();
      assert_can_rebind(port);
      expect_stopped_snapshot(server, Vajra::lifecycle::StopReason::programmatic_stop);
    }

    void test_repeated_start_stop_cycles_remain_reusable()
    {
      for (int cycle = 0; cycle < 3; ++cycle)
      {
        const int port = available_port();
        Vajra::Server server(port, "0.0.0.0", Vajra::request::kDefaultMaxRequestHeadBytes);
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

        wait_until_listening(port);
        server.stop();
        server_thread.join();

        if (server_error)
        {
          std::rethrow_exception(server_error);
        }

        assert_can_rebind(port);
        expect_stopped_snapshot(server, Vajra::lifecycle::StopReason::programmatic_stop);
      }
    }

    void test_repeated_stop_requests_are_idempotent()
    {
      const int port = available_port();
      Vajra::Server server(port, "0.0.0.0", Vajra::request::kDefaultMaxRequestHeadBytes);
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

      wait_until_listening(port);
      server.stop();
      server.stop();
      server_thread.join();

      if (server_error)
      {
        std::rethrow_exception(server_error);
      }

      assert_can_rebind(port);
      expect_stopped_snapshot(server, Vajra::lifecycle::StopReason::programmatic_stop);
    }

    void test_inherited_listener_start_and_stop_release_listener()
    {
      Vajra::listener::Socket listener_socket;
      Vajra::listener::SocketBinding binding = listener_socket.open("0.0.0.0", available_port());
      Vajra::Server server(
          binding.port,
          "0.0.0.0",
          Vajra::request::kDefaultMaxRequestHeadBytes,
          nullptr,
          "native_runtime_control",
          "master_worker",
          1,
          "ruby_worker_bootstrap",
          false,
          binding.fd);
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

      wait_until_listening(binding.port);
      server.stop();
      server_thread.join();

      if (server_error)
      {
        std::rethrow_exception(server_error);
      }

      assert_can_rebind(binding.port);
      expect_stopped_snapshot(server, Vajra::lifecycle::StopReason::programmatic_stop);
    }

    void test_stop_interrupts_keep_alive_connection()
    {
      run_stop_interrupt_test(
          [&](int port, Vajra::Server &server) {
            FileDescriptorGuard client_socket(connect_to_listener(port));
            if (client_socket.get() < 0)
            {
              fail("failed to connect keep-alive test client");
            }

            suppress_sigpipe(client_socket.get());
            if (!send_all(
                    client_socket.get(),
                    "GET / HTTP/1.1\r\n"
                    "Host: localhost\r\n"
                    "\r\n"))
            {
              fail("failed to send keep-alive request");
            }
            read_http_response(client_socket.get());

            server.stop();
            const bool peer_closed = peer_closed_within(client_socket.get(), 1000);
            client_socket.close_if_open();
            if (!peer_closed)
            {
              fail("server did not interrupt keep-alive client socket during drain");
            }
          },
          "keep-alive interrupt test could not obtain a reusable listener port after retries");
    }

    void test_stop_interrupts_partial_next_request()
    {
      run_stop_interrupt_test(
          [&](int port, Vajra::Server &server) {
            FileDescriptorGuard client_socket(connect_to_listener(port));
            if (client_socket.get() < 0)
            {
              fail("failed to connect partial-next-request test client");
            }

            suppress_sigpipe(client_socket.get());
            if (!send_all(
                    client_socket.get(),
                    "GET / HTTP/1.1\r\n"
                    "Host: localhost\r\n"
                    "\r\n"))
            {
              fail("failed to send initial keep-alive request");
            }
            read_http_response(client_socket.get());

            if (!send_all(
                    client_socket.get(),
                    "GET /next HTTP/1.1\r\n"
                    "Host: localhost\r\n"))
            {
              fail("failed to send partial next request");
            }

            server.stop();
            const bool peer_closed = peer_closed_within(client_socket.get(), 1000);
            client_socket.close_if_open();
            if (!peer_closed)
            {
              fail("server did not interrupt partial next request during drain");
            }
          },
          "partial-next-request interrupt test could not obtain a reusable listener port after retries");
    }
  }

  void run_server_lifecycle_tests()
  {
    test_start_and_stop_release_listener();
    test_stop_before_start_exits_cleanly();
    test_repeated_start_stop_cycles_remain_reusable();
    test_repeated_stop_requests_are_idempotent();
    test_inherited_listener_start_and_stop_release_listener();
    test_stop_interrupts_keep_alive_connection();
    test_stop_interrupts_partial_next_request();
  }
}
