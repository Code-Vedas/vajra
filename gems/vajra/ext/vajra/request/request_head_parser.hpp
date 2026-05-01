// Copyright Codevedas Inc. 2025-present
//
// This source code is licensed under the MIT license found in the
// LICENSE file in the root directory of this source tree.

#ifndef VAJRA_REQUEST_HEAD_PARSER_HPP
#define VAJRA_REQUEST_HEAD_PARSER_HPP

#include "request_head_error.hpp"
#include "request_head_types.hpp"
#include "request_line_validation_pipeline.hpp"

namespace Vajra
{
  namespace request
  {
    class RequestHeadParser
    {
    public:
      ParsedRequest parse(const std::string &request_head) const
      {
        const std::size_t request_line_end = request_head.find("\r\n");
        if (request_line_end == std::string::npos)
        {
          throw bad_request_error("missing request line terminator");
        }

        const std::string request_line = request_head.substr(0, request_line_end);
        const RequestLineTokens tokens{request_line.find(' '), request_line.find(' ', request_line.find(' ') + 1)};
        request_line_validators_.validate(request_line, tokens);

        ParsedRequest parsed_request{build_request_line(request_line, tokens), {}};
        parse_headers(request_head, request_line_end + 2, parsed_request);
        return parsed_request;
      }

    private:
      ParsedRequestLine build_request_line(const std::string &request_line, const RequestLineTokens &tokens) const
      {
        return ParsedRequestLine{
            request_line.substr(0, tokens.first_space),
            request_line.substr(tokens.first_space + 1, tokens.second_space - tokens.first_space - 1),
            request_line.substr(tokens.second_space + 1)};
      }

      void parse_headers(const std::string &request_head, std::size_t cursor, ParsedRequest &parsed_request) const
      {
        while (cursor < request_head.size())
        {
          const std::size_t line_end = request_head.find("\r\n", cursor);
          if (line_end == std::string::npos)
          {
            throw bad_request_error("unterminated header line");
          }

          if (line_end == cursor)
          {
            return;
          }

          parsed_request.headers.push_back(parse_header_line(request_head.substr(cursor, line_end - cursor)));
          cursor = line_end + 2;
        }

        throw bad_request_error("missing header terminator");
      }

      ParsedHeader parse_header_line(const std::string &header_line) const
      {
        const std::size_t delimiter = header_line.find(':');
        if (delimiter == std::string::npos || delimiter == 0)
        {
          throw bad_request_error("invalid header line");
        }

        const std::string header_name = header_line.substr(0, delimiter);
        if (header_name.find_first_of(" \t") != std::string::npos)
        {
          throw bad_request_error("invalid header name");
        }

        return ParsedHeader{header_name, strip_leading_header_whitespace(header_line.substr(delimiter + 1))};
      }

      std::string strip_leading_header_whitespace(std::string value) const
      {
        const std::size_t first_non_whitespace = value.find_first_not_of(" \t");
        if (first_non_whitespace == std::string::npos)
        {
          return "";
        }

        return value.substr(first_non_whitespace);
      }

      RequestLineValidationPipeline request_line_validators_;
    };
  }
}

#endif
