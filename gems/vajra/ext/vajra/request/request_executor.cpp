// Copyright Codevedas Inc. 2025-present
//
// This source code is licensed under the MIT license found in the
// LICENSE file in the root directory of this source tree.

#include "request_executor.hpp"

#include <stdexcept>
#include <utility>

#ifdef VAJRA_RUNTIME_TESTING
namespace
{
  class TestRequestExecutionSession final : public Vajra::request::RequestExecutionSession
  {
  public:
    TestRequestExecutionSession(
        const Vajra::request::RequestExecutor &request_executor,
        Vajra::request::RequestContext request_context)
        : request_executor_(request_executor),
          request_context_(std::move(request_context))
    {
    }

    void append_request_body_bytes(const char *data, std::size_t length) override
    {
      request_context_.request_body.append(data, length);
    }

    std::optional<Vajra::response::Response> finish() override
    {
      return request_executor_.execute(request_context_);
    }

  private:
    const Vajra::request::RequestExecutor &request_executor_;
    Vajra::request::RequestContext request_context_;
  };
}
#endif

std::optional<Vajra::response::Response> Vajra::request::RequestExecutor::control_response(
    const RequestContext &) const
{
  return std::nullopt;
}

bool Vajra::request::RequestExecutor::async_execution_supported() const
{
  return false;
}

bool Vajra::request::RequestExecutor::async_completion_supported() const
{
  return false;
}

std::unique_ptr<Vajra::request::RequestExecutionSession> Vajra::request::RequestExecutor::start(
    const RequestContext &request_context) const
{
#ifdef VAJRA_RUNTIME_TESTING
  return std::make_unique<TestRequestExecutionSession>(*this, request_context);
#else
  (void)request_context;
  throw std::logic_error("request executor must override start() for streaming request bodies");
#endif
}

std::optional<Vajra::response::Response> Vajra::request::RequestExecutor::execute(
    const RequestContext &) const
{
  throw std::logic_error("request executor must override execute() or start()");
}

std::optional<Vajra::response::Response> Vajra::request::RequestExecutor::execute(
    RequestContext &&request_context) const
{
  return execute(static_cast<const RequestContext &>(request_context));
}

bool Vajra::request::RequestExecutor::execute_async(
    RequestContext &&request_context,
    CompletionCallback callback) const
{
  (void)request_context;
  (void)callback;
  return false;
}
