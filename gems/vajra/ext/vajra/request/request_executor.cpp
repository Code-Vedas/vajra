// Copyright Codevedas Inc. 2025-present
//
// This source code is licensed under the MIT license found in the
// LICENSE file in the root directory of this source tree.

#include "request_executor.hpp"

#include <memory>
#include <stdexcept>

namespace
{
  class BufferedRequestExecutionSession final : public Vajra::request::RequestExecutionSession
  {
  public:
    BufferedRequestExecutionSession(
        const Vajra::request::RequestExecutor &request_executor,
        Vajra::request::RequestContext request_context)
        : request_executor_(request_executor),
          request_context_(std::move(request_context))
    {
    }

    void append_request_body_chunk(const std::string &chunk) override
    {
      request_context_.request_body.append(chunk);
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

std::optional<Vajra::response::Response> Vajra::request::RequestExecutor::control_response(
    const RequestContext &) const
{
  return std::nullopt;
}

std::unique_ptr<Vajra::request::RequestExecutionSession> Vajra::request::RequestExecutor::start(
    const RequestContext &request_context) const
{
  return std::make_unique<BufferedRequestExecutionSession>(*this, request_context);
}

std::optional<Vajra::response::Response> Vajra::request::RequestExecutor::execute(
    const RequestContext &) const
{
  throw std::logic_error("request executor must override execute() or start()");
}
