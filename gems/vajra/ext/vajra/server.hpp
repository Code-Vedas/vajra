// Copyright Codevedas Inc. 2025-present
//
// This source code is licensed under the MIT license found in the
// LICENSE file in the root directory of this source tree.

#ifndef VAJRA_SERVER_HPP
#define VAJRA_SERVER_HPP

#include <cstddef>
#include <functional>
#include <atomic>

#include "lifecycle/lifecycle_controller.hpp"
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
    lifecycle::Snapshot lifecycle_snapshot() const;
    void set_lifecycle_observer(lifecycle::Controller::Observer observer);

  private:
    int port_;
    std::atomic<int> server_fd_;
    listener::Socket listener_socket_;
    request::RequestProcessor request_processor_;
    lifecycle::Controller lifecycle_;

    void close_listener_fd(bool interrupt_accept);
  };
}

#endif
