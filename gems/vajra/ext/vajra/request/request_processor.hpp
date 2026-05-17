// Copyright Codevedas Inc. 2025-present
//
// This source code is licensed under the MIT license found in the
// LICENSE file in the root directory of this source tree.

#ifndef VAJRA_REQUEST_PROCESSOR_HPP
#define VAJRA_REQUEST_PROCESSOR_HPP

#include "request_context.hpp"
#include "request_body_reader.hpp"
#include "request_executor.hpp"
#include "request_head_parser.hpp"
#include "request_head_reader.hpp"
#include "response/response_writer.hpp"

#include <cstddef>
#include <exception>
#include <memory>

namespace Vajra
{
  namespace request
  {
    class RequestProcessor
    {
    public:
      explicit RequestProcessor(
          std::size_t max_request_head_bytes,
          std::shared_ptr<const RequestExecutor> request_executor = nullptr);

      void handle(int client_fd, const SocketContext &socket_context) const;

    private:
      Vajra::response::ConnectionBehavior connection_behavior_for(const ParsedRequest &request) const;
      Vajra::response::Response response_for(
          const RequestContext &request_context,
          Vajra::response::ConnectionBehavior connection_behavior) const;
      void reject_request_head(int client_fd, const HeadError &error) const;
      void reject_request_execution(int client_fd, const std::exception &error) const;

      HeadReader request_head_reader_;
      RequestHeadParser request_head_parser_;
      RequestBodyReader request_body_reader_;
      Vajra::response::ResponseWriter response_writer_;
      std::shared_ptr<const RequestExecutor> request_executor_;
    };
  }
}

#endif
