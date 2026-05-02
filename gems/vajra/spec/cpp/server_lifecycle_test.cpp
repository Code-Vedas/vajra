// Copyright Codevedas Inc. 2025-present
//
// This source code is licensed under the MIT license found in the
// LICENSE file in the root directory of this source tree.

#include "request/request_head_size_validator.hpp"
#include "server.hpp"
#include "test_suites.hpp"
#include "test_support.hpp"

#include <exception>
#include <thread>

namespace VajraSpecCpp
{
  namespace
  {
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
  }

  void run_server_lifecycle_tests()
  {
    test_start_and_stop_release_listener();
    test_stop_before_start_exits_cleanly();
  }
}
