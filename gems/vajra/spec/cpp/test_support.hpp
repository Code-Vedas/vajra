// Copyright Codevedas Inc. 2025-present
//
// This source code is licensed under the MIT license found in the
// LICENSE file in the root directory of this source tree.

#ifndef VAJRA_SPEC_CPP_TEST_SUPPORT_HPP
#define VAJRA_SPEC_CPP_TEST_SUPPORT_HPP

#include "request/request_head_error.hpp"
#include "request/request_head_parser.hpp"
#include "request/request_head_reader.hpp"
#include "request/request_head_types.hpp"
#include "response/response.hpp"

#include <cstddef>
#include <exception>
#include <string>
#include <vector>

namespace VajraSpecCpp
{
  [[noreturn]] void fail(const std::string &message);

  class FileDescriptorGuard
  {
  public:
    explicit FileDescriptorGuard(int fd);
    FileDescriptorGuard(const FileDescriptorGuard &) = delete;
    FileDescriptorGuard &operator=(const FileDescriptorGuard &) = delete;
    ~FileDescriptorGuard();

    int get() const;
    void close_if_open();

  private:
    int fd_;
  };

  struct ReaderOutcome
  {
    Vajra::request::HeadReadResult result;
    std::exception_ptr error;
  };

  bool bind_conflict(const std::exception_ptr &error);
  int available_port();
  int connect_to_listener(int port);
  void suppress_sigpipe(int fd);
  bool send_all(int fd, const std::string &payload);
  bool complete_probe_request(int fd);
  std::string read_all(int fd);
  std::size_t parse_content_length(const std::string &response);
  std::string read_http_response(int fd);
  bool peer_closed_within(int fd, int timeout_ms);
  void wait_until_listening(int port);
  void assert_can_rebind(int port);
  ReaderOutcome read_request_head_from_chunks(
      const std::vector<std::string> &chunks,
      std::size_t max_request_head_bytes);
  void expect_parse_success(
      const std::string &request_head,
      const std::string &expected_method,
      const std::string &expected_target,
      const std::string &expected_version,
      std::size_t expected_header_count);
  void expect_parse_error(
      const std::string &request_head,
      Vajra::request::HeadFailureKind expected_kind,
      const std::string &expected_message);
  void expect_reader_error(
      const std::vector<std::string> &chunks,
      std::size_t max_request_head_bytes,
      Vajra::request::HeadFailureKind expected_kind,
      const std::string &expected_message);
  std::string send_response_through_socket(const Vajra::response::Response &response);
}

#endif
