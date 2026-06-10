// Copyright Codevedas Inc. 2025-present
//
// This source code is licensed under the MIT license found in the
// LICENSE file in the root directory of this source tree.

#ifndef VAJRA_RUNTIME_TRACEPARENT_HPP
#define VAJRA_RUNTIME_TRACEPARENT_HPP

#include <cstddef>
#include <string>

namespace Vajra
{
  namespace runtime
  {
    inline bool traceparent_hex_value(const std::string &value, std::size_t expected_length, bool require_non_zero)
    {
      if (value.size() != expected_length)
      {
        return false;
      }

      bool non_zero = false;
      for (const char character : value)
      {
        const bool hex =
            (character >= '0' && character <= '9') ||
            (character >= 'a' && character <= 'f') ||
            (character >= 'A' && character <= 'F');
        if (!hex)
        {
          return false;
        }
        non_zero = non_zero || character != '0';
      }
      return !require_non_zero || non_zero;
    }

    inline std::string traceparent_part(const std::string &traceparent, int part)
    {
      const std::size_t first = traceparent.find('-');
      const std::size_t second = first == std::string::npos ? std::string::npos : traceparent.find('-', first + 1);
      const std::size_t third = second == std::string::npos ? std::string::npos : traceparent.find('-', second + 1);
      if (first != 2 || second == std::string::npos || third == std::string::npos)
      {
        return "";
      }

      const std::string version = traceparent.substr(0, first);
      const std::string trace_id = traceparent.substr(first + 1, second - first - 1);
      const std::string span_id = traceparent.substr(second + 1, third - second - 1);
      const std::string flags = traceparent.substr(third + 1);
      if (!traceparent_hex_value(version, 2, false) ||
          !traceparent_hex_value(trace_id, 32, true) ||
          !traceparent_hex_value(span_id, 16, true) ||
          !traceparent_hex_value(flags, 2, false))
      {
        return "";
      }

      if (part == 1)
      {
        return trace_id;
      }
      if (part == 2)
      {
        return span_id;
      }
      return part == 3 ? flags : "";
    }
  }
}

#endif
