// Copyright Codevedas Inc. 2025-present
//
// This source code is licensed under the MIT license found in the
// LICENSE file in the root directory of this source tree.

#include "request_processor.hpp"

#include <unistd.h>

namespace
{
  class ClientSocketGuard
  {
  public:
    explicit ClientSocketGuard(int fd) : fd_(fd) {}

    ~ClientSocketGuard()
    {
      if (fd_ >= 0)
      {
        close(fd_);
      }
    }

  private:
    int fd_;
  };
}

Vajra::request::RequestProcessor::RequestProcessor(std::size_t max_request_head_bytes)
    : request_head_reader_(max_request_head_bytes),
      request_head_parser_(),
      response_writer_()
{
}

void Vajra::request::RequestProcessor::handle(int client_fd) const
{
  ClientSocketGuard client_socket_guard(client_fd);

  HeadReadResult read_result;
  try
  {
    read_result = request_head_reader_.read(client_fd);
  }
  catch (const HeadError &error)
  {
    reject_request_head(client_fd, error);
    return;
  }

  if (!read_result.complete)
  {
    return;
  }

  try
  {
    (void)request_head_parser_.parse(read_result.request_head);
  }
  catch (const HeadError &error)
  {
    reject_request_head(client_fd, error);
    return;
  }

  (void)response_writer_.send_success_response(client_fd);
}

void Vajra::request::RequestProcessor::reject_request_head(int client_fd, const HeadError &error) const
{
  response_writer_.log_request_head_error(error);
  response_writer_.send_request_head_failure_response(client_fd, error.kind());
}
