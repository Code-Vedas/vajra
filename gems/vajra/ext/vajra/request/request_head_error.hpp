// Copyright Codevedas Inc. 2025-present
//
// This source code is licensed under the MIT license found in the
// LICENSE file in the root directory of this source tree.

#ifndef VAJRA_REQUEST_HEAD_ERROR_HPP
#define VAJRA_REQUEST_HEAD_ERROR_HPP

#include <cstddef>
#include <stdexcept>
#include <string>

namespace Vajra
{
  namespace request
  {
    constexpr std::size_t kDefaultMaxRequestHeadBytes = 16 * 1024;

    enum class HeadFailureKind
    {
      bad_request,
      header_too_large
    };

    class HeadError : public std::runtime_error
    {
    public:
      HeadError(HeadFailureKind kind, const std::string &message)
          : std::runtime_error(message), kind_(kind)
      {
      }

      HeadFailureKind kind() const
      {
        return kind_;
      }

    private:
      HeadFailureKind kind_;
    };

    inline HeadError bad_request_error(const std::string &message)
    {
      return HeadError(HeadFailureKind::bad_request, "request parsing failed: " + message);
    }

    inline HeadError request_head_too_large_error()
    {
      return HeadError(
          HeadFailureKind::header_too_large,
          "request parsing failed: request head exceeds maximum size");
    }
  }
}

#endif
