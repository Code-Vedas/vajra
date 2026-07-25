// Copyright Codevedas Inc. 2025-present
//
// This source code is licensed under the MIT license found in the
// LICENSE file in the root directory of this source tree.

#ifndef VAJRA_REQUEST_LINE_VALIDATION_PIPELINE_HPP
#define VAJRA_REQUEST_LINE_VALIDATION_PIPELINE_HPP

#include "request_head_error.hpp"

#include <cstddef>
#include <string>

namespace Vajra
{
  namespace request
  {
    struct RequestLineTokens
    {
      std::size_t first_space;
      std::size_t second_space;
    };

    class RequestLineStructureValidator
    {
    public:
      void validate(const std::string &request_line, const RequestLineTokens &tokens) const
      {
        if (tokens.first_space == std::string::npos || tokens.first_space == 0)
        {
          throw bad_request_error("invalid request line");
        }

        if (tokens.second_space == std::string::npos || tokens.second_space == tokens.first_space + 1)
        {
          throw bad_request_error("invalid request line");
        }

        if (request_line.find(' ', tokens.second_space + 1) != std::string::npos)
        {
          throw bad_request_error("invalid request line");
        }
      }
    };

    class HttpVersionValidator
    {
    public:
      void validate(const std::string &request_line, const RequestLineTokens &tokens) const
      {
        const std::string version = request_line.substr(tokens.second_space + 1);
        if (version != "HTTP/1.1" && version != "HTTP/1.0")
        {
          throw bad_request_error("invalid HTTP version");
        }
      }
    };

    class RequestMethodValidator
    {
    public:
      void validate(const std::string &request_line, const RequestLineTokens &tokens) const
      {
        for (std::size_t index = 0; index < tokens.first_space; ++index)
        {
          if (!is_token_character(static_cast<unsigned char>(request_line[index])))
          {
            throw bad_request_error("invalid request method");
          }
        }
      }

    private:
      static bool is_token_character(unsigned char character)
      {
        if ((character >= '0' && character <= '9') ||
            (character >= 'A' && character <= 'Z') ||
            (character >= 'a' && character <= 'z'))
        {
          return true;
        }

        const std::string token_punctuation = "!#$%&'*+-.^_`|~";
        return token_punctuation.find(static_cast<char>(character)) != std::string::npos;
      }
    };

    class RequestTargetValidator
    {
    public:
      void validate(const std::string &request_line, const RequestLineTokens &tokens) const
      {
        for (std::size_t index = tokens.first_space + 1; index < tokens.second_space; ++index)
        {
          const unsigned char character = static_cast<unsigned char>(request_line[index]);
          if (!is_uri_character(character))
          {
            throw bad_request_error("invalid request target");
          }
          if (character == '%')
          {
            if (index + 2 >= tokens.second_space ||
                !is_hex_digit(static_cast<unsigned char>(request_line[index + 1])) ||
                !is_hex_digit(static_cast<unsigned char>(request_line[index + 2])))
            {
              throw bad_request_error("invalid request target");
            }
            index += 2;
          }
        }
      }

    private:
      static bool is_hex_digit(unsigned char character)
      {
        return (character >= '0' && character <= '9') ||
               (character >= 'A' && character <= 'F') ||
               (character >= 'a' && character <= 'f');
      }

      static bool is_uri_character(unsigned char character)
      {
        if ((character >= '0' && character <= '9') ||
            (character >= 'A' && character <= 'Z') ||
            (character >= 'a' && character <= 'z'))
        {
          return true;
        }
        const std::string uri_punctuation = "-._~:/?[]@!$&'()*+,;=%";
        return uri_punctuation.find(static_cast<char>(character)) != std::string::npos;
      }
    };

    class RequestLineValidationPipeline
    {
    public:
      void validate(const std::string &request_line, const RequestLineTokens &tokens) const
      {
        structure_validator_.validate(request_line, tokens);
        method_validator_.validate(request_line, tokens);
        target_validator_.validate(request_line, tokens);
        http_version_validator_.validate(request_line, tokens);
      }

    private:
      RequestLineStructureValidator structure_validator_;
      RequestMethodValidator method_validator_;
      RequestTargetValidator target_validator_;
      HttpVersionValidator http_version_validator_;
    };
  }
}

#endif
