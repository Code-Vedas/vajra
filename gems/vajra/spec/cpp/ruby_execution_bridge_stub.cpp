// Copyright Codevedas Inc. 2025-present
//
// This source code is licensed under the MIT license found in the
// LICENSE file in the root directory of this source tree.

#include "../../ext/vajra/request/rack_env.hpp"
#include "../../ext/vajra/request/request_context.hpp"
#include "../../ext/vajra/response/response.hpp"

#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

using VALUE = unsigned long;
using ID = unsigned long;

namespace Vajra
{
  namespace transport
  {
    class TlsConnection;
  }

  namespace rack
  {
    std::shared_ptr<NativeHijackTransport> tls_native_hijack_transport(
        Vajra::transport::TlsConnection &connection);

    class RubyExecutionBridge
    {
    public:
      static void initialize();
      static ID call_id();
      static VALUE binary_string_from(const std::string &value);
      static VALUE env_entries_array_from(const std::vector<Vajra::request::RackEnvEntry> &env_entries);
      static VALUE rack_env_from(
          const std::vector<Vajra::request::RackEnvEntry> &env_entries,
          const std::string &request_body);
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

std::shared_ptr<Vajra::rack::NativeHijackTransport> Vajra::rack::tls_native_hijack_transport(
    Vajra::transport::TlsConnection &)
{
  return nullptr;
}

void Vajra::rack::RubyExecutionBridge::initialize() {}

ID Vajra::rack::RubyExecutionBridge::call_id()
{
  return 0;
}

VALUE Vajra::rack::RubyExecutionBridge::binary_string_from(const std::string &)
{
  throw std::logic_error("Ruby bridge is unavailable in native C++ tests");
}

VALUE Vajra::rack::RubyExecutionBridge::env_entries_array_from(
    const std::vector<Vajra::request::RackEnvEntry> &)
{
  throw std::logic_error("Ruby bridge is unavailable in native C++ tests");
}

VALUE Vajra::rack::RubyExecutionBridge::rack_env_from(
    const std::vector<Vajra::request::RackEnvEntry> &,
    const std::string &)
{
  throw std::logic_error("Ruby bridge is unavailable in native C++ tests");
}

Vajra::response::Response Vajra::rack::RackResponseHandler::response_from_normalized_result(VALUE)
{
  throw std::logic_error("Ruby bridge is unavailable in native C++ tests");
}

Vajra::response::Response Vajra::rack::RackResponseHandler::response_from_rack_result(VALUE)
{
  throw std::logic_error("Ruby bridge is unavailable in native C++ tests");
}

std::string Vajra::rack::RubyExecutionBridge::exception_message(VALUE)
{
  return "Ruby bridge is unavailable in native C++ tests";
}
