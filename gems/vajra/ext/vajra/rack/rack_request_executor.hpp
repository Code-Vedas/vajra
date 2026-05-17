// Copyright Codevedas Inc. 2025-present
//
// This source code is licensed under the MIT license found in the
// LICENSE file in the root directory of this source tree.

#ifndef VAJRA_RACK_REQUEST_EXECUTOR_HPP
#define VAJRA_RACK_REQUEST_EXECUTOR_HPP

#include "request/rack_env.hpp"
#include "request/request_executor.hpp"
#include "ruby.h"

#include <memory>
#include <vector>

namespace Vajra
{
  namespace rack
  {
    void initialize_rack_execution_bridge();
    void set_rack_execution_callback(VALUE callback);

    class RackExecutionTransport
    {
    public:
      virtual ~RackExecutionTransport() = default;
      virtual std::optional<Vajra::response::Response> execute(
          const std::vector<request::RackEnvEntry> &env_entries,
          const std::string &request_body) const = 0;
    };

    class RackRequestExecutor final : public request::RequestExecutor
    {
    public:
      RackRequestExecutor();
      explicit RackRequestExecutor(std::shared_ptr<const RackExecutionTransport> transport);

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
