// Copyright Codevedas Inc. 2025-present
//
// This source code is licensed under the MIT license found in the
// LICENSE file in the root directory of this source tree.

#ifndef VAJRA_REQUEST_HEAD_TYPES_HPP
#define VAJRA_REQUEST_HEAD_TYPES_HPP

#include <string>
#include <vector>

namespace Vajra
{
  namespace request
  {
    struct ParsedHeader
    {
      std::string name;
      std::string value;
    };

    struct ParsedRequestLine
    {
      std::string method;
      std::string target;
      std::string version;
    };

    struct ParsedRequest
    {
      ParsedRequestLine request_line;
      std::vector<ParsedHeader> headers;
    };
  }
}

#endif
