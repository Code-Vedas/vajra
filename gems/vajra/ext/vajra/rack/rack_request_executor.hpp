// Copyright Codevedas Inc. 2025-present
//
// This source code is licensed under the MIT license found in the
// LICENSE file in the root directory of this source tree.

#ifndef VAJRA_RACK_REQUEST_EXECUTOR_HPP
#define VAJRA_RACK_REQUEST_EXECUTOR_HPP

#include "request/request_executor.hpp"
#include "ruby.h"

namespace Vajra
{
  namespace rack
  {
    void initialize_rack_execution_bridge();
    void set_rack_execution_callback(VALUE callback);

    class RackRequestExecutor final : public request::RequestExecutor
    {
    public:
      std::optional<Vajra::response::Response> execute(const request::RequestContext &request_context) const override;
    };
  }
}

#endif
