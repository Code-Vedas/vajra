// Copyright Codevedas Inc. 2025-present
//
// This source code is licensed under the MIT license found in the
// LICENSE file in the root directory of this source tree.

#ifndef VAJRA_RESPONSE_HPP
#define VAJRA_RESPONSE_HPP

#include <cstdio>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace Vajra
{
  namespace response
  {
    enum class ConnectionBehavior
    {
      keep_alive,
      close,
    };

    struct Header
    {
      std::string name;
      std::string value;
    };

    struct Status
    {
      int code;
      std::string reason_phrase;
    };

    struct ResponseBodyFile
    {
      explicit ResponseBodyFile(FILE *body_file) : file(body_file) {}
      ResponseBodyFile(const ResponseBodyFile &) = delete;
      ResponseBodyFile &operator=(const ResponseBodyFile &) = delete;
      ~ResponseBodyFile()
      {
        if (file != nullptr)
        {
          std::fclose(file);
          file = nullptr;
        }
      }

      FILE *file = nullptr;
      std::size_t size = 0;
    };

    struct Response
    {
      Response() = default;

      Response(
          Status response_status,
          std::vector<Header> response_headers,
          std::string response_body,
          ConnectionBehavior response_connection_behavior = ConnectionBehavior::close)
          : status(std::move(response_status)),
            headers(std::move(response_headers)),
            body(std::move(response_body)),
            connection_behavior(response_connection_behavior)
      {
      }

      Response(
          Status response_status,
          std::vector<Header> response_headers,
          std::vector<std::string> response_body_chunks,
          ConnectionBehavior response_connection_behavior = ConnectionBehavior::close)
          : status(std::move(response_status)),
            headers(std::move(response_headers)),
            connection_behavior(response_connection_behavior),
            body_chunks(std::move(response_body_chunks))
      {
      }

      Status status;
      std::vector<Header> headers;
      std::string body;
      ConnectionBehavior connection_behavior = ConnectionBehavior::close;
      std::vector<std::string> body_chunks;
      std::shared_ptr<ResponseBodyFile> body_file;
      bool hijacked = false;
    };

    inline bool response_has_body_chunks(const Response &response)
    {
      return !response.body_chunks.empty();
    }

    inline bool response_has_body_file(const Response &response)
    {
      return response.body_file && response.body_file->file != nullptr;
    }

    inline std::size_t response_body_size(const Response &response)
    {
      if (response_has_body_file(response))
      {
        return response.body_file->size;
      }
      if (!response_has_body_chunks(response))
      {
        return response.body.size();
      }

      std::size_t size = 0;
      for (const std::string &chunk : response.body_chunks)
      {
        size += chunk.size();
      }
      return size;
    }

    inline bool response_body_empty(const Response &response)
    {
      return response_body_size(response) == 0;
    }
  }
}

#endif
