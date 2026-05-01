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
        if (request_line.substr(tokens.second_space + 1) != "HTTP/1.1")
        {
          throw bad_request_error("invalid HTTP version");
        }
      }
    };

    class RequestLineValidationPipeline
    {
    public:
      void validate(const std::string &request_line, const RequestLineTokens &tokens) const
      {
        structure_validator_.validate(request_line, tokens);
        http_version_validator_.validate(request_line, tokens);
      }

    private:
      RequestLineStructureValidator structure_validator_;
      HttpVersionValidator http_version_validator_;
    };
  }
}

#endif
