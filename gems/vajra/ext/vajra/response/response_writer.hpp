// Copyright Codevedas Inc. 2025-present
//
// This source code is licensed under the MIT license found in the
// LICENSE file in the root directory of this source tree.

#ifndef VAJRA_RESPONSE_WRITER_HPP
#define VAJRA_RESPONSE_WRITER_HPP

#include "request/request_head_error.hpp"
#include "response.hpp"
#include "response_serializer.hpp"
#include "transport/connection.hpp"

#include <string>

namespace Vajra
{
  namespace response
  {
    class ResponseWriter
    {
    public:
      static void prepare_client_socket(int client_fd);
      bool send(int client_fd, const Response &response) const;
      bool send(Vajra::transport::Connection &connection, const Response &response) const;
      bool send(Vajra::transport::Connection &connection, const Response &response, bool suppress_body) const;
      Response success_response(ConnectionBehavior connection_behavior = ConnectionBehavior::close) const;
      Response internal_server_error_response() const;
      Response queue_capacity_response() const;
      Response request_timeout_response() const;
      Response request_head_failure_response(Vajra::request::HeadFailureKind kind) const;
      void log_request_head_error(const Vajra::request::HeadError &error) const;

    private:
      bool send_response_message(int client_fd, const std::string &response_message) const;
      bool send_response_message(Vajra::transport::Connection &connection, const std::string &response_message) const;
      bool send_response_bytes(Vajra::transport::Connection &connection, const char *data, std::size_t length) const;
      const char *request_head_failure_label(Vajra::request::HeadFailureKind kind) const;
      void log_serialization_error(const SerializationError &error) const;

      ResponseSerializer serializer_;
    };
  }
}

#endif
