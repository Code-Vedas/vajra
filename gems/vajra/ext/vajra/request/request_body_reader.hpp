// Copyright Codevedas Inc. 2025-present
//
// This source code is licensed under the MIT license found in the
// LICENSE file in the root directory of this source tree.

#ifndef VAJRA_REQUEST_BODY_READER_HPP
#define VAJRA_REQUEST_BODY_READER_HPP

#include "request_head_types.hpp"
#include "transport/connection.hpp"

#include <cstddef>
#include <functional>
#include <stdexcept>
#include <string>

namespace Vajra
{
  namespace request
  {
    constexpr std::size_t kDefaultMaxRequestBodyBytes = 16 * 1024 * 1024;
    constexpr std::size_t kDefaultDirectRequestBodyBytes = 1024 * 1024;
    constexpr std::size_t kDefaultMaxChunkLineBytes = 8 * 1024;
    constexpr std::size_t kDefaultMaxTrailerLineBytes = 8 * 1024;
    constexpr int kDefaultRequestBodyTimeoutSeconds = 30;

    struct BodyReadResult
    {
      std::string body;
      std::string remaining_buffered_bytes;
    };

    using BodyChunkCallback = std::function<void(const char *data, std::size_t length)>;

    enum class BodyFraming
    {
      none,
      content_length,
      chunked
    };

    struct BodyReadPlan
    {
      BodyFraming framing = BodyFraming::none;
      std::size_t content_length = 0;
    };

    class BodyReadIncompleteError : public std::runtime_error
    {
    public:
      BodyReadIncompleteError() : std::runtime_error("request body read incomplete")
      {
      }
    };

    class RequestBodyReader
    {
    public:
      explicit RequestBodyReader(
          std::size_t max_request_body_bytes = kDefaultMaxRequestBodyBytes,
          std::size_t max_chunk_line_bytes = kDefaultMaxChunkLineBytes,
          std::size_t max_trailer_line_bytes = kDefaultMaxTrailerLineBytes,
          int request_body_timeout_seconds = kDefaultRequestBodyTimeoutSeconds)
          : max_request_body_bytes_(max_request_body_bytes),
            max_chunk_line_bytes_(max_chunk_line_bytes),
            max_trailer_line_bytes_(max_trailer_line_bytes),
            request_body_timeout_seconds_(request_body_timeout_seconds)
      {
      }

      BodyReadResult stream_read(
          int client_fd,
          const ParsedRequest &request,
          const BodyChunkCallback &on_body_chunk,
          std::string buffered_bytes = "") const;
      BodyReadResult stream_read(
          Vajra::transport::Connection &connection,
          const ParsedRequest &request,
          const BodyChunkCallback &on_body_chunk,
          std::string buffered_bytes = "") const;

      BodyReadResult read(
          int client_fd,
          const ParsedRequest &request,
          std::string buffered_bytes = "") const;
      BodyReadResult read(
          Vajra::transport::Connection &connection,
          const ParsedRequest &request,
          std::string buffered_bytes = "") const;
      BodyReadPlan plan_for(const ParsedRequest &request) const;
      bool can_read_without_streaming(
          const BodyReadPlan &plan,
          const std::string &buffered_bytes) const;

    private:
      std::size_t max_request_body_bytes_;
      std::size_t max_chunk_line_bytes_;
      std::size_t max_trailer_line_bytes_;
      int request_body_timeout_seconds_;
    };
  }
}

#endif
