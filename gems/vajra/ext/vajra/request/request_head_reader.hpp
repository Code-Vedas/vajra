// Copyright Codevedas Inc. 2025-present
//
// This source code is licensed under the MIT license found in the
// LICENSE file in the root directory of this source tree.

#ifndef VAJRA_REQUEST_HEAD_READER_HPP
#define VAJRA_REQUEST_HEAD_READER_HPP

#include "request_head_size_validator.hpp"

#include <cstddef>
#include <string>

namespace Vajra
{
  namespace request
  {
    struct HeadReadResult
    {
      bool complete;
      std::string request_head;
    };

    class HeadReader
    {
    public:
      explicit HeadReader(std::size_t max_request_head_bytes);

      HeadReadResult read(int client_fd) const;

    private:
      bool configure_read_timeout(int client_fd) const;

      RequestHeadSizeValidator request_head_size_validator_;
    };
  }
}

#endif
