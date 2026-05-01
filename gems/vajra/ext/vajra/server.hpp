// Copyright Codevedas Inc. 2025-present
//
// This source code is licensed under the MIT license found in the
// LICENSE file in the root directory of this source tree.

#ifndef SERVER_HPP
#define SERVER_HPP

#include <atomic>
#include <cstddef>
#include "listener/listener_socket.hpp"
#include "request/request_head_error.hpp"
#include "request/request_processor.hpp"

namespace Vajra
{
  class Server
  {
  public:
    explicit Server(int port, std::size_t max_request_head_bytes = request::kDefaultMaxRequestHeadBytes);
    ~Server();

    void start();
    void stop();

  private:
    int port_;
    std::atomic<int> server_fd_;
    std::atomic<bool> running_;
    std::atomic<bool> stop_requested_;
    listener::Socket listener_socket_;
    request::RequestProcessor request_processor_;

    void close_listener_fd(bool interrupt_accept);
  };
}

#endif
