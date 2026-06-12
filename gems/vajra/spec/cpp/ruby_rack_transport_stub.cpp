// Copyright Codevedas Inc. 2025-present
//
// This source code is licensed under the MIT license found in the
// LICENSE file in the root directory of this source tree.

#include "../../ext/vajra/rack/ruby_rack_transport.hpp"

#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace
{
  class TestRackExecutionSession final : public Vajra::rack::RackExecutionSession
  {
  public:
    TestRackExecutionSession(
        const Vajra::rack::RackExecutionTransport &transport,
        std::vector<Vajra::request::RackEnvEntry> env_entries)
        : transport_(transport),
          env_entries_(std::move(env_entries))
    {
    }

    void append_request_body_bytes(const char *data, std::size_t length) override
    {
      request_body_.append(data, length);
    }

    std::optional<Vajra::response::Response> finish() override
    {
      return transport_.execute(env_entries_, request_body_, -1, nullptr, nullptr);
    }

  private:
    const Vajra::rack::RackExecutionTransport &transport_;
    std::vector<Vajra::request::RackEnvEntry> env_entries_;
    std::string request_body_;
  };

  class RubyRackTransportStub final : public Vajra::rack::RackExecutionTransport
  {
  public:
    std::optional<Vajra::response::Response> execute(
        const std::vector<Vajra::request::RackEnvEntry> &,
        const std::string &,
        int,
        std::shared_ptr<Vajra::rack::Http2StreamState>,
        std::shared_ptr<Vajra::rack::NativeHijackTransport>) const override
    {
      return std::nullopt;
    }
  };
}

std::unique_ptr<Vajra::rack::RackExecutionSession> Vajra::rack::RackExecutionTransport::start(
    const std::vector<request::RackEnvEntry> &env_entries,
    int,
    std::shared_ptr<NativeHijackTransport>) const
{
  return std::make_unique<TestRackExecutionSession>(*this, env_entries);
}

std::string Vajra::rack::RackExecutionTransport::stats_payload_json() const
{
  return "{}";
}

std::string Vajra::rack::RackExecutionTransport::metrics_payload_text() const
{
  return "";
}

std::shared_ptr<const Vajra::rack::RackExecutionTransport> Vajra::rack::same_process_rack_execution_transport()
{
  return std::make_shared<RubyRackTransportStub>();
}

std::optional<Vajra::response::Response> Vajra::rack::execute_current_thread_rack_request(
    const std::vector<Vajra::request::RackEnvEntry> &,
    const std::string &,
    int)
{
  throw std::logic_error("Ruby Rack transport is unavailable in native C++ tests");
}
