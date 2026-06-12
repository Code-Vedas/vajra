// Copyright Codevedas Inc. 2025-present
//
// This source code is licensed under the MIT license found in the
// LICENSE file in the root directory of this source tree.

#ifndef VAJRA_REQUEST_EXECUTOR_HPP
#define VAJRA_REQUEST_EXECUTOR_HPP

#include "request_context.hpp"
#include "response/response.hpp"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>

namespace Vajra
{
  namespace rack
  {
    struct NativeInputState;
  }

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
      virtual Vajra::rack::NativeInputState *native_input_state() { return nullptr; }
      virtual std::shared_ptr<Vajra::rack::NativeInputState> native_input_state_owner() { return nullptr; }
      virtual void append_request_body_bytes(const char *data, std::size_t length) = 0;
      virtual bool try_append_request_body_bytes(const char *data, std::size_t length)
      {
        append_request_body_bytes(data, length);
        return true;
      }
      virtual void finish_request_body() {}
      virtual void fail_request_body(const std::string &) noexcept {}
      virtual std::optional<Vajra::response::Response> finish() = 0;
    };

    class RequestExecutor
    {
    public:
      using CompletionCallback = std::function<void(
          std::optional<Vajra::response::Response>,
          std::string,
          std::int64_t)>;

      virtual ~RequestExecutor() = default;
      virtual bool async_execution_supported() const;
      virtual bool async_completion_supported() const;
      virtual std::optional<Vajra::response::Response> control_response(const RequestContext &request_context) const;
      virtual std::unique_ptr<RequestExecutionSession> start(const RequestContext &request_context) const;
      virtual std::optional<Vajra::response::Response> execute(const RequestContext &request_context) const;
      virtual std::optional<Vajra::response::Response> execute(RequestContext &&request_context) const;
      virtual bool execute_async(RequestContext &&request_context, CompletionCallback callback) const;
    };
  }
}

#endif
