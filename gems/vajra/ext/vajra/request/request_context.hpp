// Copyright Codevedas Inc. 2025-present
//
// This source code is licensed under the MIT license found in the
// LICENSE file in the root directory of this source tree.

#ifndef VAJRA_REQUEST_CONTEXT_HPP
#define VAJRA_REQUEST_CONTEXT_HPP

#include "request_head_types.hpp"

#include <string>

namespace Vajra
{
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
      std::string request_body = "";
    };
  }
}

#endif
