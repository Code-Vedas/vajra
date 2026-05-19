// Copyright Codevedas Inc. 2025-present
//
// This source code is licensed under the MIT license found in the
// LICENSE file in the root directory of this source tree.

#ifndef VAJRA_REQUEST_EXECUTOR_HPP
#define VAJRA_REQUEST_EXECUTOR_HPP

#include "request_context.hpp"
#include "response/response.hpp"

#include <memory>
#include <optional>
#include <stdexcept>

namespace Vajra
{
  namespace request
  {
    class QueueCapacityError : public std::runtime_error
    {
    public:
      explicit QueueCapacityError(const std::string &message)
          : std::runtime_error(message)
      {
      }
    };

    class RequestTimeoutError : public std::runtime_error
    {
    public:
      explicit RequestTimeoutError(const std::string &message)
          : std::runtime_error(message)
      {
      }
    };

    class RequestExecutionSession
    {
    public:
      virtual ~RequestExecutionSession() = default;
      virtual void append_request_body_chunk(const std::string &chunk) = 0;
      virtual std::optional<Vajra::response::Response> finish() = 0;
    };

    class RequestExecutor
    {
    public:
      virtual ~RequestExecutor() = default;
      virtual std::unique_ptr<RequestExecutionSession> start(const RequestContext &request_context) const;
      virtual std::optional<Vajra::response::Response> execute(const RequestContext &request_context) const;
    };
  }
}

#endif
