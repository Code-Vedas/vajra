// Copyright Codevedas Inc. 2025-present
//
// This source code is licensed under the MIT license found in the
// LICENSE file in the root directory of this source tree.

#ifndef SERVER_HPP
#define SERVER_HPP

#include <atomic>
#include <string>
#include <vector>

struct ParsedHeader
{
  std::string name;
  std::string value;
};

struct ParsedRequestLine
{
  std::string method;
  std::string target;
  std::string version;
};

struct ParsedRequest
{
  ParsedRequestLine request_line;
  std::vector<ParsedHeader> headers;
};

ParsedRequest parse_request_head(const std::string &request_head);

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
  std::atomic<bool> stop_requested_;

  void setup_socket();
  void close_listener_fd(bool interrupt_accept);
};

#endif
