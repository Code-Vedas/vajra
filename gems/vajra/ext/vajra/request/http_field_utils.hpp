// Copyright Codevedas Inc. 2025-present
//
// This source code is licensed under the MIT license found in the
// LICENSE file in the root directory of this source tree.

#ifndef VAJRA_REQUEST_HTTP_FIELD_UTILS_HPP
#define VAJRA_REQUEST_HTTP_FIELD_UTILS_HPP

#include <string>

namespace Vajra
{
  namespace request
  {
    inline bool ascii_case_equal(char left, char right)
    {
      if (left >= 'A' && left <= 'Z')
      {
        left = static_cast<char>(left - 'A' + 'a');
      }

      if (right >= 'A' && right <= 'Z')
      {
        right = static_cast<char>(right - 'A' + 'a');
      }

      return left == right;
    }

    inline bool ascii_case_insensitive_equal(const std::string &left, const std::string &right)
    {
      if (left.size() != right.size())
      {
        return false;
      }

      for (std::size_t index = 0; index < left.size(); ++index)
      {
        if (!ascii_case_equal(left[index], right[index]))
        {
          return false;
        }
      }

      return true;
    }

    inline std::string strip_http_whitespace(const std::string &value)
    {
      const std::size_t start = value.find_first_not_of(" \t");
      if (start == std::string::npos)
      {
        return "";
      }

      const std::size_t end = value.find_last_not_of(" \t");
      return value.substr(start, end - start + 1);
    }

    inline bool content_length_is_zero(const std::string &value)
    {
      const std::string normalized = strip_http_whitespace(value);
      if (normalized.empty())
      {
        return false;
      }

      for (const char character : normalized)
      {
        if (character < '0' || character > '9')
        {
          return false;
        }

        if (character != '0')
        {
          return false;
        }
      }

      return true;
    }
  }
}

#endif
