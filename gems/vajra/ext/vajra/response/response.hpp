// Copyright Codevedas Inc. 2025-present
//
// This source code is licensed under the MIT license found in the
// LICENSE file in the root directory of this source tree.

#ifndef VAJRA_RESPONSE_HPP
#define VAJRA_RESPONSE_HPP

#include <string>
#include <vector>

namespace Vajra
{
  namespace response
  {
    struct Header
    {
      std::string name;
      std::string value;
    };

    struct Status
    {
      int code;
      std::string reason_phrase;
    };

    struct Response
    {
      Status status;
      std::vector<Header> headers;
      std::string body;
    };
  }
}

#endif
