// Copyright Codevedas Inc. 2025-present
//
// This source code is licensed under the MIT license found in the
// LICENSE file in the root directory of this source tree.

#ifndef VAJRA_STATIC_RESPONSE_WRITER_HPP
#define VAJRA_STATIC_RESPONSE_WRITER_HPP

#include "request/request_head_error.hpp"

namespace Vajra
{
  namespace response
  {
    class StaticResponseWriter
    {
    public:
      bool send_success_response(int client_fd) const;
      void send_request_head_failure_response(int client_fd, Vajra::request::HeadFailureKind kind) const;
      void log_request_head_error(const Vajra::request::HeadError &error) const;

    private:
      bool send_response_message(int client_fd, const char *response) const;
      const char *request_head_failure_label(Vajra::request::HeadFailureKind kind) const;
      const char *request_head_failure_response(Vajra::request::HeadFailureKind kind) const;
    };
  }
}

#endif
