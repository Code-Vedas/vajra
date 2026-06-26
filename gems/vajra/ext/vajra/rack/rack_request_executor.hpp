// Copyright Codevedas Inc. 2025-present
//
// This source code is licensed under the MIT license found in the
// LICENSE file in the root directory of this source tree.

#ifndef VAJRA_RACK_REQUEST_EXECUTOR_HPP
#define VAJRA_RACK_REQUEST_EXECUTOR_HPP

#include "request/rack_env.hpp"
#include "request/request_executor.hpp"

#ifdef VAJRA_RUNTIME_TESTING
using VALUE = unsigned long;
#else
#include "ruby.h"
#endif

#include <cstddef>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace Vajra
{
  namespace rack
  {
    struct NativeInputState;
    struct Http2StreamState;
    class NativeHijackTransport;

    void initialize_rack_execution_bridge();
    void set_rack_execution_callback(VALUE callback);
    void set_rack_execution_app(VALUE app);
    class RackExecutionSession
    {
    public:
      virtual ~RackExecutionSession() = default;
      virtual NativeInputState *native_input_state() { return nullptr; }
      virtual std::shared_ptr<NativeInputState> native_input_state_owner() { return nullptr; }
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

    class RackExecutionTransport
    {
    public:
      virtual ~RackExecutionTransport() = default;
      virtual bool async_execution_supported() const { return false; }
      virtual bool async_completion_supported() const { return false; }
      virtual std::unique_ptr<RackExecutionSession> start(
          const std::vector<request::RackEnvEntry> &env_entries,
          int client_fd,
          std::shared_ptr<NativeHijackTransport> native_hijack_transport = nullptr) const;
      virtual std::optional<Vajra::response::Response> execute(
          const std::vector<request::RackEnvEntry> &env_entries,
          const std::string &request_body,
          int client_fd,
          std::shared_ptr<Http2StreamState> http2_stream = nullptr,
          std::shared_ptr<NativeHijackTransport> native_hijack_transport = nullptr) const = 0;
      virtual std::optional<Vajra::response::Response> execute(
          const std::vector<request::RackEnvEntry> &env_entries,
          std::string &&request_body,
          int client_fd,
          std::shared_ptr<Http2StreamState> http2_stream = nullptr,
          std::shared_ptr<NativeHijackTransport> native_hijack_transport = nullptr) const
      {
        return execute(
            env_entries,
            static_cast<const std::string &>(request_body),
            client_fd,
            std::move(http2_stream),
            std::move(native_hijack_transport));
      }
      virtual bool execute_async(
          std::vector<request::RackEnvEntry> env_entries,
          std::string request_body,
          int client_fd,
          std::shared_ptr<Http2StreamState> http2_stream,
          std::shared_ptr<NativeHijackTransport> native_hijack_transport,
          request::RequestExecutor::CompletionCallback callback) const
      {
        (void)env_entries;
        (void)request_body;
        (void)client_fd;
        (void)http2_stream;
        (void)native_hijack_transport;
        (void)callback;
        return false;
      }
      virtual std::string stats_payload_json() const;
      virtual std::string metrics_payload_text() const;
    };

    struct ControlPlaneConfig
    {
      std::string stats_path;
      std::string metrics_endpoint;
    };

    class RackRequestExecutor final : public request::RequestExecutor
    {
    public:
      RackRequestExecutor();
      explicit RackRequestExecutor(
          std::shared_ptr<const RackExecutionTransport> transport,
          ControlPlaneConfig control_plane_config = {});

      std::optional<Vajra::response::Response> control_response(
          const request::RequestContext &request_context) const override;
      bool async_execution_supported() const override;
      std::unique_ptr<request::RequestExecutionSession> start(
          const request::RequestContext &request_context) const override;
      std::optional<Vajra::response::Response> execute(const request::RequestContext &request_context) const override;
      std::optional<Vajra::response::Response> execute(request::RequestContext &&request_context) const override;
      bool async_completion_supported() const override;
      bool execute_async(
          request::RequestContext &&request_context,
          request::RequestExecutor::CompletionCallback callback) const override;

    private:
      std::shared_ptr<const RackExecutionTransport> transport_;
      ControlPlaneConfig control_plane_config_;
    };
  }
}

#endif
