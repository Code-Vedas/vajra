// Copyright Codevedas Inc. 2025-present
//
// This source code is licensed under the MIT license found in the
// LICENSE file in the root directory of this source tree.

#ifndef VAJRA_REQUEST_CONTEXT_HPP
#define VAJRA_REQUEST_CONTEXT_HPP

#include "request_head_types.hpp"

#include <memory>
#include <string>

namespace Vajra
{
  namespace rack
  {
    struct Http2StreamState;
    class NativeHijackTransport;
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
      ParsedRequest request;
      SocketContext socket;
      int client_fd = -1;
      std::string request_body = "";
      std::shared_ptr<Vajra::rack::Http2StreamState> http2_stream;
      std::shared_ptr<Vajra::rack::NativeHijackTransport> native_hijack_transport;
    };
  }
}

#endif
