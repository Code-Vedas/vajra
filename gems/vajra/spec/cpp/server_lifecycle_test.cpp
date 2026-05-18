// Copyright Codevedas Inc. 2025-present
//
// This source code is licensed under the MIT license found in the
// LICENSE file in the root directory of this source tree.

#include "request/request_head_size_validator.hpp"
#include "server.hpp"
#include "test_suites.hpp"
#include "test_support.hpp"

#include <exception>
#include <unistd.h>
#include <sstream>
#include <thread>

namespace VajraSpecCpp
{
  namespace
  {
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
      Vajra::Server server(port, Vajra::request::kDefaultMaxRequestHeadBytes);
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
      Vajra::listener::SocketBinding binding = listener_socket.open(available_port());
      Vajra::Server server(
          binding.port,
          Vajra::request::kDefaultMaxRequestHeadBytes,
          nullptr,
          "ruby_worker_bootstrap",
          "master_worker",
          1,
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
  }

  void run_server_lifecycle_tests()
  {
    test_start_and_stop_release_listener();
    test_stop_before_start_exits_cleanly();
    test_repeated_start_stop_cycles_remain_reusable();
    test_repeated_stop_requests_are_idempotent();
    test_inherited_listener_start_and_stop_release_listener();
  }
}
