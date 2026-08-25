// Copyright Codevedas Inc. 2025-present
//
// This source code is licensed under the MIT license found in the
// LICENSE file in the root directory of this source tree.

#ifndef VAJRA_REQUEST_HTTP2_SESSION_HPP
#define VAJRA_REQUEST_HTTP2_SESSION_HPP

#include "request_context.hpp"
#include "request_body_reader.hpp"
#include "request_executor.hpp"
#include "response/response.hpp"
#include "transport/connection.hpp"

#include <cstdint>
#include <cstddef>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace Vajra
{
  namespace request
  {
    struct Http2Config
    {
      static constexpr std::size_t kDefaultConnectionBufferBytes = 16 * 1024 * 1024;

      std::size_t max_concurrent_streams = 128;
      std::size_t initial_window_size = 1'048'576;
      std::size_t max_frame_size = 1'048'576;
      std::size_t header_table_size = 4'096;
      std::size_t max_request_head_bytes = 64 * 1024;
      std::size_t max_request_body_bytes = kDefaultMaxRequestBodyBytes;
      std::size_t max_connection_buffer_bytes = kDefaultConnectionBufferBytes;
      std::size_t max_keepalive_requests = 0;
      std::size_t max_pending_executions = 0;

      // Runtime construction intentionally assigns every member by name.  It
      // prevents new configuration fields from shifting an aggregate
      // initializer in a platform-specific worker path.
      static Http2Config from_runtime_options(
          std::size_t runtime_max_concurrent_streams,
          std::size_t runtime_initial_window_size,
          std::size_t runtime_max_frame_size,
          std::size_t runtime_header_table_size,
          std::size_t runtime_max_request_head_bytes,
          std::size_t runtime_max_request_body_bytes,
          std::size_t runtime_max_keepalive_requests,
          std::size_t runtime_max_pending_executions,
          std::size_t runtime_max_connection_buffer_bytes = kDefaultConnectionBufferBytes)
      {
        Http2Config config;
        config.max_concurrent_streams = runtime_max_concurrent_streams;
        config.initial_window_size = runtime_initial_window_size;
        config.max_frame_size = runtime_max_frame_size;
        config.header_table_size = runtime_header_table_size;
        config.max_request_head_bytes = runtime_max_request_head_bytes;
        config.max_request_body_bytes = runtime_max_request_body_bytes;
        config.max_connection_buffer_bytes = runtime_max_connection_buffer_bytes;
        config.max_keepalive_requests = runtime_max_keepalive_requests;
        config.max_pending_executions = runtime_max_pending_executions;
        return config;
      }
    };

    struct Http2UpgradeRequest
    {
      RequestContext request_context;
      std::vector<std::uint8_t> settings_payload;
      std::string trailing_bytes;
    };

    class Http2ExecutionPool final
    {
    public:
      explicit Http2ExecutionPool(std::size_t thread_count);
      ~Http2ExecutionPool();

      Http2ExecutionPool(const Http2ExecutionPool &) = delete;
      Http2ExecutionPool &operator=(const Http2ExecutionPool &) = delete;

      void enqueue(std::function<void()> work);

    private:
      class Impl;

      std::unique_ptr<Impl> impl_;
    };

    class Http2Session final
    {
    public:
      Http2Session(
          Vajra::transport::Connection &connection,
          SocketContext socket_context,
          Http2Config config,
          std::shared_ptr<const RequestExecutor> request_executor,
          std::shared_ptr<Http2ExecutionPool> execution_pool);
      Http2Session(
          Vajra::transport::Connection &connection,
          SocketContext socket_context,
          Http2Config config,
          std::shared_ptr<const RequestExecutor> request_executor,
          std::shared_ptr<Http2ExecutionPool> execution_pool,
          std::string initial_bytes);
      Http2Session(
          Vajra::transport::Connection &connection,
          SocketContext socket_context,
          Http2Config config,
          std::shared_ptr<const RequestExecutor> request_executor,
          std::shared_ptr<Http2ExecutionPool> execution_pool,
          Http2UpgradeRequest upgrade_request);
      ~Http2Session();

      void run();

#ifdef VAJRA_RUNTIME_TESTING
      // Test-only visibility for the producer-lifetime admission invariant.
      // The count is read after run() has returned, so it intentionally does
      // not make the session's loop-owned collection externally mutable.
      std::size_t active_response_producer_count_for_testing() const;
#endif

    private:
      class Impl;

      std::unique_ptr<Impl> impl_;
    };
  }
}

#endif
