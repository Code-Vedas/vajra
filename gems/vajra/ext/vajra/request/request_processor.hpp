// Copyright Codevedas Inc. 2025-present
//
// This source code is licensed under the MIT license found in the
// LICENSE file in the root directory of this source tree.

#ifndef VAJRA_REQUEST_PROCESSOR_HPP
#define VAJRA_REQUEST_PROCESSOR_HPP

#include "request_context.hpp"
#include "request_body_reader.hpp"
#include "request_executor.hpp"
#include "http2_session.hpp"
#include "request_head_parser.hpp"
#include "request_head_reader.hpp"
#include "response/response_writer.hpp"
#include "transport/connection.hpp"

#include <cstddef>
#include <exception>
#include <memory>

namespace Vajra
{
    namespace request
    {
        enum class RequestProcessingOutcome
        {
            await_read,
            keep_alive,
            hijacked,
            close
        };

        struct RequestProcessingResult
        {
            RequestProcessingOutcome outcome = RequestProcessingOutcome::close;
            std::string buffered_bytes = "";
            bool first_request = true;
        };

        class RequestProcessor
        {
        public:
            RequestProcessor(
                std::size_t max_request_head_bytes,
                std::shared_ptr<const RequestExecutor> request_executor);

            explicit RequestProcessor(
                std::size_t max_request_head_bytes,
                std::size_t max_request_body_bytes = kDefaultMaxRequestBodyBytes,
                int request_head_timeout_seconds = 5,
                int first_data_timeout_seconds = 30,
                int request_body_timeout_seconds = kDefaultRequestBodyTimeoutSeconds,
                int persistent_timeout_seconds = 30,
                std::size_t max_keepalive_requests = 0,
                std::shared_ptr<const RequestExecutor> request_executor = nullptr,
                std::size_t http2_execution_threads = 4,
                bool http2_enabled = false,
                Http2Config http2_config = {});

            RequestProcessingOutcome handle(Vajra::transport::Connection &connection, const SocketContext &socket_context) const;
            std::shared_ptr<Http2ExecutionPool> http2_execution_pool() const;
            RequestProcessingResult handle_one(
                Vajra::transport::Connection &connection,
                const SocketContext &socket_context,
                std::string buffered_bytes = "",
                bool first_request = true,
                bool force_close_after_response = false) const;

        private:
            Vajra::response::ConnectionBehavior connection_behavior_for(const ParsedRequest &request) const;
            Vajra::response::Response response_for(
                RequestExecutionSession &execution_session,
                Vajra::response::ConnectionBehavior connection_behavior) const;
            Vajra::response::Response response_for(
                RequestContext &request_context,
                Vajra::response::ConnectionBehavior connection_behavior) const;
            bool reject_request_head(
                Vajra::transport::Connection &connection,
                const HeadError &error,
                const Vajra::response::Response &response) const;
            bool reject_request_queue_capacity(
                Vajra::transport::Connection &connection,
                const QueueCapacityError &error,
                const Vajra::response::Response &response) const;
            bool reject_request_timeout(
                Vajra::transport::Connection &connection,
                const RequestTimeoutError &error,
                const Vajra::response::Response &response) const;
            bool reject_request_execution(
                Vajra::transport::Connection &connection,
                const std::exception &error,
                const Vajra::response::Response &response) const;

            HeadReader request_head_reader_;
            RequestHeadParser request_head_parser_;
            RequestBodyReader request_body_reader_;
            Vajra::response::ResponseWriter response_writer_;
            std::shared_ptr<const RequestExecutor> request_executor_;
            std::shared_ptr<Http2ExecutionPool> http2_execution_pool_;
            bool http2_enabled_;
            Http2Config http2_config_;
            int first_data_timeout_seconds_;
            int persistent_timeout_seconds_;
            std::size_t max_keepalive_requests_;
        };
    }
}

#endif
