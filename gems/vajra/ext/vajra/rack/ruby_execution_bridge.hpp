// Copyright Codevedas Inc. 2025-present
//
// This source code is licensed under the MIT license found in the
// LICENSE file in the root directory of this source tree.

#ifndef VAJRA_RACK_RUBY_EXECUTION_BRIDGE_HPP
#define VAJRA_RACK_RUBY_EXECUTION_BRIDGE_HPP

#include "request/rack_env.hpp"
#include "response/response.hpp"
#include "ruby.h"

#include <string>
#include <memory>
#include <vector>

namespace Vajra
{
  namespace transport
  {
    class TlsConnection;
  }

  namespace rack
  {
    struct NativeInputState;
    struct NativeHijackState;
    struct Http2StreamState;

    class NativeHijackTransport
    {
    public:
      virtual ~NativeHijackTransport() = default;
      virtual VALUE call() = 0;
    };

    std::shared_ptr<NativeHijackTransport> tls_native_hijack_transport(
        Vajra::transport::TlsConnection &connection);

    class RubyJumpTag final
    {
    public:
      explicit RubyJumpTag(int state) : state_(state) {}
      int state() const { return state_; }

    private:
      int state_;
    };

    class RubyExecutionBridge
    {
    public:
      static void initialize();
      static ID call_id();
      static void set_multithread(bool enabled);
      static VALUE binary_string_from(const std::string &value);
      static VALUE env_entries_array_from(const std::vector<Vajra::request::RackEnvEntry> &env_entries);
      static VALUE rack_env_from(
          const std::vector<Vajra::request::RackEnvEntry> &env_entries,
          std::string request_body,
          int client_fd = -1,
          std::shared_ptr<NativeHijackState> *hijack_state = nullptr,
          std::shared_ptr<Http2StreamState> http2_stream = nullptr,
          std::shared_ptr<NativeHijackTransport> native_hijack_transport = nullptr);
      static VALUE rack_env_from(
          const std::vector<Vajra::request::RackEnvEntry> &env_entries,
          VALUE rack_input,
          int client_fd,
          std::shared_ptr<NativeInputState> input_state,
          std::shared_ptr<NativeHijackState> *hijack_state,
          std::shared_ptr<Http2StreamState> http2_stream = nullptr,
          std::shared_ptr<NativeHijackTransport> native_hijack_transport = nullptr);
      static bool native_hijack_called(const std::shared_ptr<NativeHijackState> &state);
      static void commit_native_hijack(const std::shared_ptr<NativeHijackState> &state);
      static void close_rack_input(VALUE env);
      static std::string exception_message(VALUE exception);
    };

    class RackResponseHandler
    {
    public:
      static Vajra::response::Response response_from_normalized_result(VALUE value);
      static Vajra::response::Response response_from_rack_result(VALUE value);
    };
  }
}

#endif
