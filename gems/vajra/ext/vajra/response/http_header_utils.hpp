// Copyright Codevedas Inc. 2025-present
//
// This source code is licensed under the MIT license found in the
// LICENSE file in the root directory of this source tree.

#ifndef VAJRA_RESPONSE_HTTP_HEADER_UTILS_HPP
#define VAJRA_RESPONSE_HTTP_HEADER_UTILS_HPP

#include "response.hpp"

#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

namespace Vajra
{
  namespace response
  {
    inline char lowercase_ascii(char character)
    {
      if (character >= 'A' && character <= 'Z')
      {
        return static_cast<char>(character - 'A' + 'a');
      }

      return character;
    }

    inline bool ascii_alpha_numeric(unsigned char character)
    {
      return (character >= '0' && character <= '9') || (character >= 'A' && character <= 'Z') ||
             (character >= 'a' && character <= 'z');
    }

    inline bool valid_header_name_character(unsigned char character)
    {
      if (ascii_alpha_numeric(character))
      {
        return true;
      }

      switch (character)
      {
      case '!':
      case '#':
      case '$':
      case '%':
      case '&':
      case '\'':
      case '*':
      case '+':
      case '-':
      case '.':
      case '^':
      case '_':
      case '`':
      case '|':
      case '~':
        return true;
      default:
        return false;
      }
    }

    inline bool contains_invalid_http_text_bytes(const std::string &value)
    {
      for (const unsigned char character : value)
      {
        if ((character <= 0x1F && character != '\t') || character == 0x7F)
        {
          return true;
        }
      }

      return false;
    }

    inline bool header_name_equals(std::string_view name, std::string_view expected)
    {
      if (name.size() != expected.size())
      {
        return false;
      }

      for (std::size_t index = 0; index < name.size(); ++index)
      {
        if (lowercase_ascii(name[index]) != expected[index])
        {
          return false;
        }
      }

      return true;
    }

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
      return header_name_equals(name, "content-length") ||
             header_name_equals(name, "connection") ||
             header_name_equals(name, "transfer-encoding");
    }

    inline bool http2_forbidden_response_header_name(const std::string &name)
    {
      return framing_header_name(name) ||
             header_name_equals(name, "keep-alive") ||
             header_name_equals(name, "proxy-connection") ||
             header_name_equals(name, "upgrade");
    }

    inline bool status_forbids_message_body(int status_code)
    {
      return (status_code >= 100 && status_code < 200) || status_code == 204 || status_code == 205 ||
             status_code == 304;
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
