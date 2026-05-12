// Copyright Codevedas Inc. 2025-present
//
// This source code is licensed under the MIT license found in the
// LICENSE file in the root directory of this source tree.

#ifndef VAJRA_RESPONSE_HTTP_HEADER_UTILS_HPP
#define VAJRA_RESPONSE_HTTP_HEADER_UTILS_HPP

#include "response.hpp"

#include <string>
#include <vector>

namespace Vajra
{
  namespace response
  {
    inline std::string lowercase_header_name(const std::string &name)
    {
      std::string normalized;
      normalized.reserve(name.size());

      for (const unsigned char character : name)
      {
        if (character >= 'A' && character <= 'Z')
        {
          normalized.push_back(static_cast<char>(character - 'A' + 'a'));
        }
        else
        {
          normalized.push_back(static_cast<char>(character));
        }
      }

      return normalized;
    }

    inline bool framing_header_name(const std::string &name)
    {
      const std::string normalized_name = lowercase_header_name(name);
      return normalized_name == "content-length" || normalized_name == "connection" ||
             normalized_name == "transfer-encoding";
    }

    inline std::vector<Header> strip_framing_headers(const std::vector<Header> &headers)
    {
      std::vector<Header> filtered_headers;
      filtered_headers.reserve(headers.size());

      for (const Header &header : headers)
      {
        if (!framing_header_name(header.name))
        {
          filtered_headers.push_back(header);
        }
      }

      return filtered_headers;
    }
  }
}

#endif
