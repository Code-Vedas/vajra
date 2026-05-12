// Copyright Codevedas Inc. 2025-present
//
// This source code is licensed under the MIT license found in the
// LICENSE file in the root directory of this source tree.

#ifndef VAJRA_REQUEST_EXECUTOR_HPP
#define VAJRA_REQUEST_EXECUTOR_HPP

#include "request_context.hpp"
#include "response/response.hpp"

#include <optional>

namespace Vajra
{
  namespace request
  {
    class RequestExecutor
    {
    public:
      virtual ~RequestExecutor() = default;
      virtual std::optional<Vajra::response::Response> execute(const RequestContext &request_context) const = 0;
    };
  }
}

#endif
