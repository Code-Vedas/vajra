// Copyright Codevedas Inc. 2025-present
//
// This source code is licensed under the MIT license found in the
// LICENSE file in the root directory of this source tree.

#ifndef VAJRA_RESPONSE_WRITER_HPP
#define VAJRA_RESPONSE_WRITER_HPP

#include "request/request_head_error.hpp"
#include "response.hpp"
#include "response_serializer.hpp"

#include <string>

namespace Vajra
{
  namespace response
  {
    class ResponseWriter
    {
    public:
      bool send(int client_fd, const Response &response) const;
      Response success_response() const;
      Response request_head_failure_response(Vajra::request::HeadFailureKind kind) const;
      void log_request_head_error(const Vajra::request::HeadError &error) const;

    private:
      void suppress_sigpipe(int client_fd) const;
      bool send_response_message(int client_fd, const std::string &response_message) const;
      const char *request_head_failure_label(Vajra::request::HeadFailureKind kind) const;
      void log_serialization_error(const SerializationError &error) const;

      ResponseSerializer serializer_;
    };
  }
}

#endif
