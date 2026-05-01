// Copyright Codevedas Inc. 2025-present
//
// This source code is licensed under the MIT license found in the
// LICENSE file in the root directory of this source tree.

#ifndef VAJRA_REQUEST_PROCESSOR_HPP
#define VAJRA_REQUEST_PROCESSOR_HPP

#include "request_head_parser.hpp"
#include "request_head_reader.hpp"
#include "response/static_response_writer.hpp"

namespace Vajra
{
  namespace request
  {
    class RequestProcessor
    {
    public:
      explicit RequestProcessor(std::size_t max_request_head_bytes);

      void handle(int client_fd) const;

    private:
      void reject_request_head(int client_fd, const HeadError &error) const;

      HeadReader request_head_reader_;
      RequestHeadParser request_head_parser_;
      Vajra::response::StaticResponseWriter response_writer_;
    };
  }
}

#endif
