// Copyright Codevedas Inc. 2025-present
//
// This source code is licensed under the MIT license found in the
// LICENSE file in the root directory of this source tree.

#ifndef VAJRA_REQUEST_BODY_READER_HPP
#define VAJRA_REQUEST_BODY_READER_HPP

#include "request_head_types.hpp"

#include <string>

namespace Vajra
{
  namespace request
  {
    struct BodyReadResult
    {
      std::string body;
      std::string remaining_buffered_bytes;
    };

    class RequestBodyReader
    {
    public:
      BodyReadResult read(
          int client_fd,
          const ParsedRequest &request,
          std::string buffered_bytes = "") const;
    };
  }
}

#endif
