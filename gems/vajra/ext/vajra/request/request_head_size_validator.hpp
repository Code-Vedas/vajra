// Copyright Codevedas Inc. 2025-present
//
// This source code is licensed under the MIT license found in the
// LICENSE file in the root directory of this source tree.

#ifndef VAJRA_REQUEST_HEAD_SIZE_VALIDATOR_HPP
#define VAJRA_REQUEST_HEAD_SIZE_VALIDATOR_HPP

#include "request_head_error.hpp"

namespace Vajra
{
  namespace request
  {
    class RequestHeadSizeValidator
    {
    public:
      explicit RequestHeadSizeValidator(std::size_t max_request_head_bytes)
          : max_request_head_bytes_(max_request_head_bytes)
      {
      }

      void validate(std::size_t request_head_bytes) const
      {
        if (request_head_bytes > max_request_head_bytes_)
        {
          throw request_head_too_large_error();
        }
      }

    private:
      std::size_t max_request_head_bytes_;
    };
  }
}

#endif
