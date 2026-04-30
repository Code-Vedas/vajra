// Copyright Codevedas Inc. 2025-present
//
// This source code is licensed under the MIT license found in the
// LICENSE file in the root directory of this source tree.

#ifndef SERVER_HPP
#define SERVER_HPP

#include <atomic>

class Server
{
public:
  explicit Server(int port);
  ~Server();

  void start();
  void stop();

private:
  int port_;
  std::atomic<int> server_fd_;
  std::atomic<bool> running_;

  void setup_socket();
  void close_listener_fd(bool interrupt_accept);
};

#endif
