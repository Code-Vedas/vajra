// Copyright Codevedas Inc. 2025-present
//
// This source code is licensed under the MIT license found in the
// LICENSE file in the root directory of this source tree.

#ifndef VAJRA_REQUEST_LINE_VALIDATION_PIPELINE_HPP
#define VAJRA_REQUEST_LINE_VALIDATION_PIPELINE_HPP

#include "request_head_error.hpp"

#include <array>
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

    class RequestLineValidationStep
    {
    public:
      virtual ~RequestLineValidationStep() = default;
      virtual void validate(const std::string &request_line, const RequestLineTokens &tokens) const = 0;
    };

    class RequestLineStructureValidator : public RequestLineValidationStep
    {
    public:
      void validate(const std::string &request_line, const RequestLineTokens &tokens) const override
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

    class HttpVersionValidator : public RequestLineValidationStep
    {
    public:
      void validate(const std::string &request_line, const RequestLineTokens &tokens) const override
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
      RequestLineValidationPipeline() : steps_{&structure_validator_, &http_version_validator_} {}

      void validate(const std::string &request_line, const RequestLineTokens &tokens) const
      {
        for (const auto *step : steps_)
        {
          step->validate(request_line, tokens);
        }
      }

    private:
      RequestLineStructureValidator structure_validator_;
      HttpVersionValidator http_version_validator_;
      std::array<const RequestLineValidationStep *, 2> steps_;
    };
  }
}

#endif
