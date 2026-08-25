// Copyright Codevedas Inc. 2025-present
//
// This source code is licensed under the MIT license found in the
// LICENSE file in the root directory of this source tree.

#ifndef VAJRA_REQUEST_CONTEXT_HPP
#define VAJRA_REQUEST_CONTEXT_HPP

#include "platform/socket.hpp"
#include "request_head_types.hpp"

#include <memory>
#include <string>
#include <utility>

namespace Vajra
{
  namespace rack
  {
    struct Http2StreamState;
    class NativeHijackTransport;
  }

  namespace response
  {
    class ConnectionBufferBudget;
  }

  namespace request
  {
    struct SocketContext
    {
      std::string remote_address;
      int remote_port;
      std::string server_name;
      int server_port;
      std::string scheme;
    };

    struct RequestContext
    {
      RequestContext() = default;

      // Keep the historic positional construction shape stable while the
      // H2-only budget remains an explicitly named trailing argument. This
      // avoids silently remapping native-hijack callers when the context
      // grows, and avoids aggregate-initializer diagnostics in embedders.
      RequestContext(
          ParsedRequest parsed_request,
          SocketContext socket_context,
          platform::SocketHandle socket_handle = platform::kInvalidSocket,
          std::string body = "",
          std::shared_ptr<Vajra::rack::Http2StreamState> h2_stream = nullptr,
          std::shared_ptr<Vajra::rack::NativeHijackTransport> hijack_transport = nullptr,
          std::shared_ptr<Vajra::response::ConnectionBufferBudget> buffer_budget = nullptr)
          : request(std::move(parsed_request)),
            socket(std::move(socket_context)),
            client_fd(socket_handle),
            request_body(std::move(body)),
            http2_stream(std::move(h2_stream)),
            native_hijack_transport(std::move(hijack_transport)),
            connection_buffer_budget(std::move(buffer_budget))
      {
      }

      ParsedRequest request;
      SocketContext socket;
      platform::SocketHandle client_fd = platform::kInvalidSocket;
      std::string request_body = "";
      std::shared_ptr<Vajra::rack::Http2StreamState> http2_stream;
      std::shared_ptr<Vajra::rack::NativeHijackTransport> native_hijack_transport;
      // Set only by the HTTP/2 session.  It is intentionally distinct from
      // http2_stream, which is the Rack-visible extended-CONNECT wrapper.
      // Appending it preserves all pre-existing positional aggregate
      // initializers used by embedders and tests.
      std::shared_ptr<Vajra::response::ConnectionBufferBudget> connection_buffer_budget;
    };
  }
}

#endif
