// Copyright Codevedas Inc. 2025-present
//
// This source code is licensed under the MIT license found in the
// LICENSE file in the root directory of this source tree.

#ifndef VAJRA_RACK_REQUEST_EXECUTOR_HPP
#define VAJRA_RACK_REQUEST_EXECUTOR_HPP

#include "request/rack_env.hpp"
#include "request/request_executor.hpp"
#include "runtime/worker_pool.hpp"
#include "ruby.h"

#include <cstddef>
#include <functional>
#include <memory>
#include <optional>
#include <vector>

namespace Vajra
{
  namespace rack
  {
    void initialize_rack_execution_bridge();
    void set_rack_execution_callback(VALUE callback);
    void run_worker_request_execution_loop(
        const std::vector<int> &channel_fds,
        std::size_t max_threads);
    std::shared_ptr<const class RackExecutionTransport> request_channel_transport(int channel_fd);
    std::shared_ptr<const class RackExecutionTransport> request_channel_transport(
        const std::vector<std::shared_ptr<Vajra::runtime::SharedWorkerState>> &worker_states,
        std::size_t min_threads,
        std::size_t queue_capacity,
        std::size_t request_timeout_seconds,
        std::size_t worker_timeout_seconds,
        std::function<void(const std::shared_ptr<Vajra::runtime::SharedWorkerState> &)> worker_timeout_handler,
        bool debug_logging);
    class RackExecutionSession
    {
    public:
      virtual ~RackExecutionSession() = default;
      virtual void append_request_body_chunk(const std::string &chunk) = 0;
      virtual std::optional<Vajra::response::Response> finish() = 0;
    };

    class RackExecutionTransport
    {
    public:
      virtual ~RackExecutionTransport() = default;
      virtual std::unique_ptr<RackExecutionSession> start(
          const std::vector<request::RackEnvEntry> &env_entries,
          int client_fd) const;
      virtual std::optional<Vajra::response::Response> execute(
          const std::vector<request::RackEnvEntry> &env_entries,
          const std::string &request_body) const = 0;
    };

    class RackRequestExecutor final : public request::RequestExecutor
    {
    public:
      RackRequestExecutor();
      explicit RackRequestExecutor(std::shared_ptr<const RackExecutionTransport> transport);

      std::unique_ptr<request::RequestExecutionSession> start(
          const request::RequestContext &request_context) const override;
      std::optional<Vajra::response::Response> execute(const request::RequestContext &request_context) const override;

    private:
      // The current same-process Ruby callback path is bootstrap transport only.
      // Later worker IPC routing should replace this transport without redefining
      // the outer Rack execution contract consumed by RequestProcessor.
      std::shared_ptr<const RackExecutionTransport> transport_;
    };
  }
}

#endif
