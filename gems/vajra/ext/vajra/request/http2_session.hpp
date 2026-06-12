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
      std::size_t max_concurrent_streams = 128;
      std::size_t initial_window_size = 1'048'576;
      std::size_t max_frame_size = 1'048'576;
      std::size_t header_table_size = 4'096;
      std::size_t max_request_head_bytes = 64 * 1024;
      std::size_t max_request_body_bytes = kDefaultMaxRequestBodyBytes;
      std::size_t max_keepalive_requests = 0;
      std::size_t max_pending_executions = 0;
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

    private:
      class Impl;

      std::unique_ptr<Impl> impl_;
    };
  }
}

#endif
